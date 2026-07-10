"""No-driver regression tests for ``mem_probe._dc``.

This module loads the source file directly and replaces every OS/kernel
boundary with a fake.  It must never import or start the rt_io helper.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import threading
import time
import types
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
DC_SOURCE = ROOT / "python" / "mem_probe" / "_dc.py"


def load_dc():
    name = f"_sao_dc_test_{uuid.uuid4().hex}"
    spec = importlib.util.spec_from_file_location(name, DC_SOURCE)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load _dc.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(name, None)
        raise
    return module


class FakeMemProbe:
    def __init__(self, rt_io) -> None:
        self.module = types.ModuleType("mem_probe")
        self.module.rt_io = rt_io
        self.previous = None

    def __enter__(self):
        self.previous = sys.modules.get("mem_probe")
        sys.modules["mem_probe"] = self.module
        return self.module

    def __exit__(self, *_exc):
        if self.previous is None:
            sys.modules.pop("mem_probe", None)
        else:
            sys.modules["mem_probe"] = self.previous


class DisplayContextNoDriverTests(unittest.TestCase):
    def test_calibration_failure_is_retryable(self) -> None:
        dc = load_dc()
        results = iter([
            (-1, -1, -1, -1, -1),
            (0x18, 0x58, 0x40, 0x48, 0x38),
        ])
        dc._calibrate = lambda: next(results)

        self.assertFalse(dc._ensure_cal())
        self.assertTrue(dc._ensure_cal())
        self.assertEqual(dc._exs_off, 0x18)

    def test_set_window_pos_stub_call_is_serialized(self) -> None:
        dc = load_dc()
        dc._swp_syscall_fn = lambda *_args: 1
        dc._swp_mem = 0x1000
        dc._build_swp_syscall = lambda: True
        active = 0
        max_active = 0
        state_lock = threading.Lock()

        def arm(_mem):
            nonlocal active, max_active
            with state_lock:
                active += 1
                max_active = max(max_active, active)
            time.sleep(0.03)
            return True

        def disarm(_mem):
            nonlocal active
            with state_lock:
                active -= 1
            return True

        dc._stub_arm = arm
        dc._stub_disarm = disarm
        barrier = threading.Barrier(3)
        results = []

        def invoke():
            barrier.wait()
            results.append(dc.syscall_set_window_pos(1, 0, 0, 0, 1, 1, 0))

        threads = [threading.Thread(target=invoke) for _ in range(2)]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join(timeout=2)

        self.assertEqual(results, [True, True])
        self.assertEqual(max_active, 1)

    def test_stub_arm_flush_failure_restores_rw_before_returning(self) -> None:
        dc = load_dc()
        protections = []

        class K32:
            def VirtualProtect(self, mem, size, protection, old):
                protections.append(int(protection))
                return True

            def FlushInstructionCache(self, *_args):
                return False

            def GetCurrentProcess(self):
                return 1

        dc._get_k32 = lambda: K32()
        self.assertFalse(dc._stub_arm(0x1000))
        self.assertEqual(protections, [dc._PAGE_RX, dc._PAGE_RW])
        self.assertNotIn(0x1000, dc._stub_poisoned)

    def test_stub_disarm_failure_prevents_false_success(self) -> None:
        dc = load_dc()
        dc._swp_syscall_fn = lambda *_args: 1
        dc._swp_mem = 0x1000
        dc._build_swp_syscall = lambda: True
        dc._stub_arm = lambda _mem: True
        dc._stub_disarm = lambda _mem: False
        self.assertFalse(
            dc.syscall_set_window_pos(1, 0, 0, 0, 1, 1, 0))

    def test_calibration_ready_cache_is_bound_to_epoch_and_build(self) -> None:
        dc = load_dc()
        epoch = [1]
        build = [22631]
        calls = []

        def calibrate():
            calls.append((epoch[0], build[0]))
            base = len(calls) * 0x10
            return (base, base + 8, 0x40, 0x48, 0x38)

        dc._rt_session_epoch = lambda: epoch[0]
        dc._windows_build = lambda: build[0]
        dc._calibrate = calibrate
        self.assertTrue(dc._ensure_cal())
        first = dc._exs_off
        self.assertTrue(dc._ensure_cal())
        self.assertEqual(len(calls), 1)
        epoch[0] = 2
        self.assertTrue(dc._ensure_cal())
        self.assertNotEqual(dc._exs_off, first)
        build[0] = 26100
        self.assertTrue(dc._ensure_cal())
        self.assertEqual(len(calls), 3)

    def test_hmvalidate_cache_is_bound_to_helper_epoch(self) -> None:
        dc = load_dc()
        epoch = [1]
        addresses = iter((0x1111, 0x2222))
        fake_rt = types.SimpleNamespace(
            session_epoch=lambda: epoch[0],
            _hmv_addr=lambda: next(addresses),
        )
        old_cfunctype = dc.ctypes.CFUNCTYPE
        dc.ctypes.CFUNCTYPE = lambda *_sig: (
            lambda address: (lambda _hwnd, _kind: address))
        dc._windows_build = lambda: 22631
        try:
            with FakeMemProbe(fake_rt):
                self.assertEqual(dc._get_tw_fallback(1), 0x1111)
                self.assertEqual(dc._get_tw_fallback(1), 0x1111)
                epoch[0] = 2
                self.assertEqual(dc._get_tw_fallback(1), 0x2222)
        finally:
            dc.ctypes.CFUNCTYPE = old_cfunctype

    def test_revoke_then_same_pid_tid_hwnd_gets_new_generation(self) -> None:
        dc = load_dc()
        dc._window_identity = lambda _hwnd: (100, 200)
        first = dc.register_window(0x44)
        self.assertIsNotNone(first)
        self.assertTrue(dc.revoke_window(first))
        second = dc.register_window(0x44)
        self.assertIsNotNone(second)
        self.assertGreater(second.generation, first.generation)

    def test_hide_window_rect_resolves_tagwnd_after_settle(self) -> None:
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._rect_off = 0x58
        token = types.SimpleNamespace(generation=3)
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: True
        tw_values = iter((0x1000, 0x2000))
        dc._get_tw = lambda _hwnd: next(tw_values)
        dc._our_cr3 = lambda: 0x3000
        old_values = (10, 20, 30, 40)
        dc._read_tw_bytes = lambda *_args: b''.join(
            value.to_bytes(4, 'little') for value in old_values)
        translated = []

        def va_to_pa(_cr3, va, *_args):
            translated.append(va)
            return va + 0x100000

        dc._va_to_pa = va_to_pa
        committed = []
        dc._pw_batch_or_fallback = (
            lambda pairs, **_kwargs: committed.append(pairs) or True)
        old = os.environ.get("SAO_HIDE_RCWINDOW")
        os.environ["SAO_HIDE_RCWINDOW"] = "1"
        try:
            self.assertTrue(dc.hide_window_rect(0x44, settle_ms=0))
        finally:
            if old is None:
                os.environ.pop("SAO_HIDE_RCWINDOW", None)
            else:
                os.environ["SAO_HIDE_RCWINDOW"] = old

        self.assertEqual(
            translated,
            [0x2000 + 0x58 + i * 4 for i in range(4)],
        )
        self.assertEqual(len(committed), 1)
        self.assertEqual(
            committed[0],
            [
                (0x100000 + 0x2000 + 0x58 + i * 4,
                 old_values[i], desired)
                for i, desired in enumerate((0, 0, 1, 1))
            ],
        )

    def test_unknown_batch_does_not_fall_back_to_individual_writes(self) -> None:
        dc = load_dc()
        calls = []
        fake_rt = types.SimpleNamespace(
            _pw_batch_tx=lambda _pairs, **_kwargs: {"outcome": "unknown"},
            _pw=lambda pa, value: calls.append((pa, value)) or True,
        )
        with FakeMemProbe(fake_rt):
            self.assertFalse(dc._pw_batch_or_fallback([(0x1000, 1), (0x2000, 2)]))
        self.assertEqual(calls, [])

    def test_hide_window_rect_rejects_reused_hwnd_generation(self) -> None:
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._rect_off = 0x58
        token = object()
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: False
        dc._get_tw = lambda _hwnd: 0x1000
        dc._our_cr3 = lambda: 0x3000
        translated = []
        dc._va_to_pa = lambda *args, **kwargs: translated.append(args) or 0x9000
        old = os.environ.get("SAO_HIDE_RCWINDOW")
        os.environ["SAO_HIDE_RCWINDOW"] = "1"
        try:
            self.assertFalse(dc.hide_window_rect(0x44, settle_ms=0))
        finally:
            if old is None:
                os.environ.pop("SAO_HIDE_RCWINDOW", None)
            else:
                os.environ["SAO_HIDE_RCWINDOW"] = old
        self.assertEqual(translated, [])

    def test_hide_exstyle_preserves_a_concurrent_style_update(self) -> None:
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._exs_off = 0x18
        token = types.SimpleNamespace(generation=7)
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: True
        dc._get_tw = lambda _hwnd: 0x1000
        reads = iter((0x28, 0xA8, 0xA0))
        dc._read_tw_bytes = lambda *_args: int(next(reads)).to_bytes(4, "little")
        dc._our_cr3 = lambda: 0x3000
        dc._va_to_pa = lambda *_args, **_kwargs: 0x5000
        writes = []
        dc._pw_batch_or_fallback = (
            lambda pairs, **_kwargs: writes.extend(pairs) or True)

        self.assertTrue(dc.hide_exstyle(0x44, 0x8))
        self.assertEqual(writes, [(0x5000, 0xA8, 0xA0)])

    def test_session_restart_between_translation_and_write_aborts_mutation(self):
        dc = load_dc()
        epoch = [7]
        dc._rt_session_epoch = lambda: epoch[0]
        dc._ensure_cal = lambda: True
        dc._exs_off = 0x18
        token = types.SimpleNamespace(generation=7)
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: True
        dc._get_tw = lambda _hwnd: 0x1000
        reads = iter((0x28, 0x28))
        dc._read_tw_bytes = lambda *_args: int(next(reads)).to_bytes(4, "little")
        dc._our_cr3 = lambda: 0x3000

        def translate(*_args, **_kwargs):
            epoch[0] = 8
            return 0x5000

        dc._va_to_pa = translate
        commits = []
        dc._pw_batch_or_fallback = (
            lambda pairs, **kwargs: commits.append((pairs, kwargs)) or True)

        self.assertFalse(dc.hide_exstyle(0x44, 0x8))
        self.assertEqual(commits, [])

    def test_revoked_generation_after_translation_never_submits_transaction(self):
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._exs_off = 0x18
        token = types.SimpleNamespace(generation=7)
        valid = [True]
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: valid[0]
        dc._get_tw = lambda _hwnd: 0x1000
        dc._read_tw_bytes = lambda *_args: (0x28).to_bytes(4, "little")
        dc._our_cr3 = lambda: 0x3000
        dc._rt_session_epoch = lambda: 7

        def translate(*_args, **_kwargs):
            valid[0] = False
            return 0x5000

        dc._va_to_pa = translate
        commits = []
        dc._pw_batch_or_fallback = (
            lambda pairs, **kwargs: commits.append((pairs, kwargs)) or True)

        self.assertFalse(dc.hide_exstyle(0x44, 0x8))
        self.assertEqual(commits, [])

    def test_hide_z_order_rejects_broken_neighbor_backlink(self) -> None:
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._spwnd_prev_off = 0x40
        dc._spwnd_next_off = 0x48
        dc._spwnd_self_off = 0x38
        token = types.SimpleNamespace(generation=11)
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: True
        tw = 0x100000
        dc._get_tw = lambda _hwnd: tw
        dc._our_cr3 = lambda: 0x3000
        dc._desktop_heap_offsets_valid = lambda *_args: True
        heap_base = tw - 0x1000
        neighbor_a = heap_base + 0x2000
        neighbor_b = heap_base + 0x3000
        values = {
            (tw, 0x38): 0x1000,
            (tw, 0x40): 0x2000,
            (tw, 0x48): 0x3000,
            (neighbor_a, 0x48): 0xDEAD,
            (neighbor_b, 0x40): 0x1000,
        }
        dc._read_tw_qword = lambda obj, off: values.get((obj, off))
        translated = []
        dc._va_to_pa = lambda *args, **kwargs: translated.append(args) or 0x5000

        self.assertFalse(dc.hide_z_order(0x44))
        self.assertEqual(translated, [])

    def test_hide_z_order_rejects_neighbors_outside_desktop_heap(self) -> None:
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._spwnd_prev_off = 0x40
        dc._spwnd_next_off = 0x48
        dc._spwnd_self_off = 0x38
        token = types.SimpleNamespace(generation=13)
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: True
        tw = 0x100000
        dc._get_tw = lambda _hwnd: tw
        dc._our_cr3 = lambda: 0x3000
        values = {
            (tw, 0x38): 0x1000,
            (tw, 0x40): 0x2000,
            (tw, 0x48): 0x3000,
        }
        dc._read_tw_qword = lambda obj, off: values.get((obj, off))
        dc._desktop_heap_offsets_valid = lambda *_args: False
        writes = []
        dc._pw_batch_or_fallback = (
            lambda pairs, **_kwargs: writes.extend(pairs) or True)
        self.assertFalse(dc.hide_z_order(0x44))
        self.assertEqual(writes, [])

    def test_hide_z_order_passes_neighbor_backlinks_as_expected_values(self):
        dc = load_dc()
        dc._ensure_cal = lambda: True
        dc._spwnd_prev_off = 0x40
        dc._spwnd_next_off = 0x48
        dc._spwnd_self_off = 0x38
        token = types.SimpleNamespace(generation=12)
        dc._capture_window_token = lambda _hwnd: token
        dc._validate_window_token = lambda _token: True
        tw = 0x100000
        dc._get_tw = lambda _hwnd: tw
        dc._our_cr3 = lambda: 0x3000
        dc._desktop_heap_offsets_valid = lambda *_args: True
        self_value = 0x1000
        prev_value = 0x2000
        next_value = 0x3000
        heap_base = tw - self_value
        neighbor_a = heap_base + prev_value
        neighbor_b = heap_base + next_value
        values = {
            (tw, 0x38): self_value,
            (tw, 0x40): prev_value,
            (tw, 0x48): next_value,
            (neighbor_a, 0x48): self_value,
            (neighbor_b, 0x40): self_value,
        }
        dc._read_tw_qword = lambda obj, off: values.get((obj, off))
        dc._va_to_pa = lambda _cr3, va, _generation=0: va + 0x500000
        committed = []
        dc._pw_batch_or_fallback = (
            lambda pairs, **_kwargs: committed.extend(pairs) or True)

        self.assertTrue(dc.hide_z_order(0x44))
        self.assertEqual(committed, [
            (neighbor_a + 0x48 + 0x500000,
             self_value & 0xFFFFFFFF, next_value & 0xFFFFFFFF),
            (neighbor_a + 0x4C + 0x500000,
             self_value >> 32, next_value >> 32),
            (neighbor_b + 0x40 + 0x500000,
             self_value & 0xFFFFFFFF, prev_value & 0xFFFFFFFF),
            (neighbor_b + 0x44 + 0x500000,
             self_value >> 32, prev_value >> 32),
        ])

    def test_our_cr3_cache_is_bound_to_proxy_session_epoch(self) -> None:
        dc = load_dc()
        epoch = [1]
        cr3_values = iter((0x111000, 0x222000))
        fake_rt = types.SimpleNamespace(
            session_epoch=lambda: epoch[0],
            _r1_fe=lambda _pid: (0xFFFF0000, next(cr3_values)),
        )
        with FakeMemProbe(fake_rt):
            self.assertEqual(dc._our_cr3(), 0x111000)
            self.assertEqual(dc._our_cr3(), 0x111000)
            epoch[0] = 2
            self.assertEqual(dc._our_cr3(), 0x222000)

    def test_va_translation_cache_is_bound_to_windows_build(self) -> None:
        dc = load_dc()
        build = [22631]
        calls = []
        fake_rt = types.SimpleNamespace(
            session_epoch=lambda: 7,
            _r1_w=lambda cr3, va: calls.append((cr3, va)) or 0x900010,
        )
        dc._windows_build = lambda: build[0]
        with FakeMemProbe(fake_rt):
            self.assertEqual(dc._va_to_pa(0x3000, 0x4010, 2), 0x900010)
            self.assertEqual(dc._va_to_pa(0x3000, 0x4010, 2), 0x900010)
            build[0] = 26100
            self.assertEqual(dc._va_to_pa(0x3000, 0x4010, 2), 0x900010)
        self.assertEqual(len(calls), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
