"""Driver-consumer ownership tests that never start the rt_io helper.

The suite replaces the driver facade with an in-memory fake before invoking
any lifecycle method.  It is therefore safe to run on a development machine
without loading, probing, or unloading a driver.
"""
from __future__ import annotations

import os
import sys
import threading
import time
import unittest
from unittest import mock


_PYTHON_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "python")
if _PYTHON_ROOT not in sys.path:
    sys.path.insert(0, _PYTHON_ROOT)


class _FakeDriver:
    ENGINE_CAPS = {"fake": 1}
    CAP_WRITE = 2
    TIER_DESC = {"A": "fake"}
    _engine = "fake"

    def __init__(self, *, ensure=True, attach=True):
        self.ensure_result = ensure
        self.attach_result = attach
        self.epoch = 1
        self.active_pid = 0
        self.ensure_calls = 0
        self.attach_calls = []
        self.detach_calls = 0

    def session_epoch(self):
        return self.epoch

    def ensure_loaded(self, *_args, **_kwargs):
        self.ensure_calls += 1
        if isinstance(self.ensure_result, list):
            return bool(self.ensure_result.pop(0))
        return bool(self.ensure_result)

    def attach(self, pid):
        self.attach_calls.append(int(pid))
        ok = self.attach_result.pop(0) if isinstance(self.attach_result, list) else self.attach_result
        if ok:
            self.active_pid = int(pid)
        return bool(ok)

    def detach(self):
        self.detach_calls += 1
        self.active_pid = 0

    def read(self, _addr, size):
        return bytes([self.active_pid & 0xFF]) * int(size)

    def _rpm(self, _handle, _addr, buf, size, p_got):
        import ctypes
        data = self.read(_addr, size)
        ctypes.memmove(buf, data, len(data))
        ctypes.cast(p_got, ctypes.POINTER(ctypes.c_size_t))[0] = len(data)
        return True

    def read_batch(self, requests):
        return [self.read(addr, size) for addr, size in requests]

    def write(self, _addr, _data):
        return self.active_pid > 0

    def memory_tier(self):
        return "A"


class TargetLeaseTests(unittest.TestCase):
    def setUp(self):
        from mem_probe import process

        self.process = process
        self.old_driver = process._drv
        self.driver = _FakeDriver()
        process._drv = self.driver
        process._reset_target_leases_for_test()

    def tearDown(self):
        self.process._reset_target_leases_for_test()
        self.process._drv = self.old_driver

    def test_same_pid_close_does_not_detach_surviving_lease(self):
        first = self.process.TargetLease(101)
        second = self.process.TargetLease(101)

        with first.operation() as drv:
            self.assertEqual(drv.read(0, 1), b"e")
        first.close()
        self.assertEqual(self.driver.detach_calls, 0)
        with second.operation() as drv:
            self.assertEqual(drv.read(0, 1), b"e")
        second.close()

        self.assertEqual(self.driver.detach_calls, 1)

    def test_different_pid_operations_explicitly_reactivate_their_owner(self):
        first = self.process.TargetLease(101)
        second = self.process.TargetLease(202)

        with first.operation() as drv:
            first_data = drv.read(0, 1)
        with second.operation() as drv:
            second_data = drv.read(0, 1)
        with first.operation() as drv:
            first_again = drv.read(0, 1)

        self.assertEqual((first_data, second_data, first_again), (b"e", b"\xca", b"e"))
        self.assertEqual(self.driver.attach_calls[-3:], [101, 202, 101])
        first.close()
        second.close()

    def test_epoch_change_forces_reattach_before_next_operation(self):
        lease = self.process.TargetLease(303)
        with lease.operation():
            pass
        self.driver.epoch += 1
        with lease.operation():
            pass
        self.assertEqual(self.driver.attach_calls, [303, 303])
        lease.close()

    def test_failed_ensure_retries_after_backoff_in_same_epoch(self):
        self.driver.ensure_result = [False, True]
        now = [10.0]
        lease = self.process.TargetLease(404)
        with mock.patch.object(self.process.time, "monotonic", side_effect=lambda: now[0]):
            self.assertFalse(lease.ensure_active())
            now[0] = 11.0
            self.assertFalse(lease.ensure_active())
            now[0] = 12.1
            self.assertTrue(lease.ensure_active())
        self.assertEqual(self.driver.ensure_calls, 2)
        lease.close()

    def test_new_epoch_clears_failed_ensure_backoff(self):
        self.driver.ensure_result = [False, True]
        now = [10.0]
        lease = self.process.TargetLease(405)
        with mock.patch.object(self.process.time, "monotonic", side_effect=lambda: now[0]):
            self.assertFalse(lease.ensure_active())
            self.driver.epoch += 1
            self.assertTrue(lease.ensure_active())
        self.assertEqual(self.driver.ensure_calls, 2)
        lease.close()

    def _game_process(self, pid):
        gp = self.process.GameProcess.__new__(self.process.GameProcess)
        gp._pid = int(pid)
        gp._handle = 0
        gp._attached_name = "fake.exe"
        gp._closed = False
        gp._target_lease = self.process.TargetLease(pid)
        gp._region_cache = None
        gp._region_cache_time = 0.0

        class _PM:
            close_calls = 0

            def close_process(self):
                self.close_calls += 1

        gp._pm = _PM()
        return gp

    def test_game_process_close_releases_only_its_own_target_reference(self):
        first = self._game_process(505)
        second = self._game_process(505)
        self.assertEqual(first.read_bytes(0x1000, 1), b"\xf9")
        first.close()
        self.assertEqual(self.driver.detach_calls, 0)
        self.assertEqual(second.read_bytes(0x1000, 1), b"\xf9")
        second.close()
        second.close()
        self.assertEqual(self.driver.detach_calls, 1)
        self.assertEqual(second._pm.close_calls, 1)

    def test_concurrent_different_pid_reads_never_observe_the_other_target(self):
        first = self.process.TargetLease(41)
        second = self.process.TargetLease(82)
        errors = []
        start = threading.Barrier(3)

        def _read_many(lease, expected):
            start.wait()
            for _ in range(100):
                with lease.operation() as drv:
                    time.sleep(0)
                    if drv.read(0, 1) != bytes([expected]):
                        errors.append((expected, drv.active_pid))

        threads = [
            threading.Thread(target=_read_many, args=(first, 41)),
            threading.Thread(target=_read_many, args=(second, 82)),
        ]
        for thread in threads:
            thread.start()
        start.wait()
        for thread in threads:
            thread.join(timeout=2.0)
        self.assertEqual(errors, [])
        first.close()
        second.close()

    def test_close_waits_for_inflight_operation_before_detach(self):
        lease = self.process.TargetLease(99)
        entered = threading.Event()
        release = threading.Event()
        closed = threading.Event()

        def _operation():
            with lease.operation():
                entered.set()
                release.wait()

        worker = threading.Thread(target=_operation)
        worker.start()
        self.assertTrue(entered.wait(timeout=1.0))
        closer = threading.Thread(target=lambda: (lease.close(), closed.set()))
        closer.start()
        self.assertFalse(closed.wait(timeout=0.05))
        self.assertEqual(self.driver.detach_calls, 0)
        release.set()
        worker.join(timeout=1.0)
        closer.join(timeout=1.0)
        self.assertTrue(closed.is_set())
        self.assertEqual(self.driver.detach_calls, 1)

    def test_shutdown_barrier_waits_for_every_lease_to_close(self):
        first = self.process.TargetLease(111)
        second = self.process.TargetLease(222)
        self.assertFalse(
            self.process._wait_for_target_leases_closed(timeout=0.01))

        result = []
        waiter = threading.Thread(target=lambda: result.append(
            self.process._wait_for_target_leases_closed(timeout=1.0)))
        waiter.start()
        first.close()
        self.assertTrue(waiter.is_alive())
        second.close()
        waiter.join(timeout=1.0)
        self.assertEqual(result, [True])

    def test_page_resolver_mp_read_is_visible_to_shutdown_barrier(self):
        from mem_probe._pm._core import _MP

        entered = threading.Event()
        release = threading.Event()

        class Resolver:
            @staticmethod
            def read_memory(_cr3, _addr, size):
                entered.set()
                release.wait(1.0)
                return b"x" * size

        lease = self.process.TargetLease(77)
        reader = _MP(77, 0x123000, Resolver(), lease)
        result = []
        worker = threading.Thread(
            target=lambda: result.append(reader.read_bytes(0x1000, 4)))
        worker.start()
        self.assertTrue(entered.wait(1.0))
        self.assertFalse(
            self.process._wait_for_target_leases_closed(timeout=0.01))
        release.set()
        worker.join(timeout=1.0)
        reader.close()
        self.assertEqual(result, [b"xxxx"])
        self.assertTrue(
            self.process._wait_for_target_leases_closed(timeout=0.1))


class SDKReaderContractTests(unittest.TestCase):
    @staticmethod
    def _install_driver(driver):
        import mem_probe
        from mem_probe import process

        old_module = sys.modules.get("mem_probe.rt_io")
        old_attr = getattr(mem_probe, "rt_io", None)
        old_process_driver = process._drv
        sys.modules["mem_probe.rt_io"] = driver
        mem_probe.rt_io = driver
        process._drv = driver
        process._reset_target_leases_for_test()
        return mem_probe, process, old_module, old_attr, old_process_driver

    @staticmethod
    def _restore_driver(state):
        mem_probe, process, old_module, old_attr, old_process_driver = state
        process._reset_target_leases_for_test()
        process._drv = old_process_driver
        if old_module is None:
            sys.modules.pop("mem_probe.rt_io", None)
        else:
            sys.modules["mem_probe.rt_io"] = old_module
        if old_attr is None:
            try:
                delattr(mem_probe, "rt_io")
            except AttributeError:
                pass
        else:
            mem_probe.rt_io = old_attr

    def test_ensure_false_is_not_reported_as_attached(self):
        from ai_editor.sdk_dumper.base import ProcessReader

        driver = _FakeDriver(ensure=False)
        state = self._install_driver(driver)
        try:
            reader = ProcessReader(77)
            with self.assertRaises(RuntimeError):
                reader._ensure_attached()
            self.assertFalse(reader._attached)
            self.assertEqual(driver.attach_calls, [])
        finally:
            self._restore_driver(state)

    def test_attach_false_is_not_reported_as_attached(self):
        from ai_editor.sdk_dumper.base import ProcessReader

        driver = _FakeDriver(attach=False)
        state = self._install_driver(driver)
        try:
            reader = ProcessReader(78)
            with self.assertRaises(RuntimeError):
                reader._ensure_attached()
            self.assertFalse(reader._attached)
        finally:
            self._restore_driver(state)

    def test_two_readers_switch_pid_without_cross_read_or_early_detach(self):
        from ai_editor.sdk_dumper.base import ProcessReader

        driver = _FakeDriver()
        state = self._install_driver(driver)
        try:
            first = ProcessReader(11)
            second = ProcessReader(22)
            self.assertEqual(first.read(0x1000, 1), b"\x0b")
            self.assertEqual(second.read(0x1000, 1), b"\x16")
            first.close()
            self.assertEqual(second.read(0x1000, 1), b"\x16")
            self.assertEqual(driver.detach_calls, 0)
            second.close()
            self.assertEqual(driver.detach_calls, 1)
        finally:
            self._restore_driver(state)


class SelectorAndViewerTests(unittest.TestCase):
    class _GP:
        def __init__(self, pid, payload=b"ABCD"):
            self.pid = pid
            self.memory_tier = "A"
            self.payload = payload
            self.close_calls = 0
            self.on_read = None
            self.alive = True

        def read_bytes(self, _addr, n):
            if self.on_read:
                self.on_read()
            return self.payload[:n]

        def is_alive(self):
            return self.alive and self.close_calls == 0

        def close(self):
            self.close_calls += 1

    def tearDown(self):
        from gui_modules import sao_gui_process_selector as selector
        selector._cache_result(None, None)

    def test_native_process_identity_uses_pid_name_and_create_time_without_handle(self):
        from mem_probe.process import query_process_identity

        identity = query_process_identity(os.getpid())
        self.assertIsNotNone(identity)
        self.assertEqual(identity[0], os.getpid())
        self.assertTrue(identity[1].endswith("python.exe"))
        self.assertGreater(identity[2], 0)

    def test_cache_replacement_closes_old_reader_and_clear_is_not_attached(self):
        from gui_modules import sao_gui_process_selector as selector

        old = self._GP(10)
        new = self._GP(20)
        self.assertTrue(selector._cache_result(None, old, name="a.exe", pid=10))
        self.assertTrue(selector._cache_result(None, new, name="b.exe", pid=20))
        self.assertEqual(old.close_calls, 1)
        selector._cache_result(None, None)
        self.assertFalse(selector.get_cached_process_info()["attached"])

    def test_selector_destroy_releases_cached_reader(self):
        from gui_modules import sao_gui_process_selector as selector

        gp = self._GP(31)
        selector._cache_result(None, gp, name="a.exe", pid=31)
        panel = selector.ProcessManagerPanel.__new__(
            selector.ProcessManagerPanel)
        panel._attach_lock = threading.Lock()
        panel._attach_threads = set()
        panel._attach_request_id = 0
        panel._destroying = False
        panel._auto_refresh_id = None
        panel._win = None
        self.assertTrue(panel.destroy())
        self.assertEqual(gp.close_calls, 1)
        self.assertFalse(selector.get_cached_process_info()["attached"])

    def test_selector_destroy_invalidates_and_drains_inflight_attach(self):
        from gui_modules import sao_gui_process_selector as selector

        panel = selector.ProcessManagerPanel.__new__(
            selector.ProcessManagerPanel)
        panel._attach_lock = threading.Lock()
        panel._attach_threads = set()
        panel._attach_request_id = 7
        panel._destroying = False
        panel._auto_refresh_id = None
        panel._win = None
        release = threading.Event()
        candidate = self._GP(32)
        request_id = panel._attach_request_id

        def late_attach():
            try:
                release.wait(1.0)
                selector._cache_result(
                    None, candidate, name="late.exe", pid=32,
                    _guard=(
                        panel._attach_lock,
                        lambda: request_id == panel._attach_request_id,
                    ),
                )
            finally:
                with panel._attach_lock:
                    panel._attach_threads.discard(threading.current_thread())

        worker = threading.Thread(target=late_attach, daemon=True)
        with panel._attach_lock:
            panel._attach_threads.add(worker)
            worker.start()
        results = []
        destroyer = threading.Thread(target=lambda: results.append(panel.destroy()))
        destroyer.start()
        deadline = time.monotonic() + 1.0
        while not panel._destroying and time.monotonic() < deadline:
            time.sleep(0.005)
        release.set()
        destroyer.join(timeout=2.0)

        self.assertEqual(results, [True])
        self.assertEqual(candidate.close_calls, 1)
        self.assertFalse(selector.get_cached_process_info()["attached"])

    def test_cache_rejects_reader_whose_pid_does_not_match_selection(self):
        from gui_modules import sao_gui_process_selector as selector

        wrong = self._GP(11)
        self.assertFalse(selector._cache_result(None, wrong, name="a.exe", pid=12))
        self.assertEqual(wrong.close_calls, 1)
        self.assertFalse(selector.get_cached_process_info()["attached"])

    def test_obsolete_attach_request_cannot_replace_newer_cached_reader(self):
        from gui_modules import sao_gui_process_selector as selector

        current = self._GP(21)
        obsolete = self._GP(22)
        selector._cache_result(None, current, name="current.exe", pid=21)
        guard = threading.Lock()
        self.assertFalse(selector._cache_result(
            None, obsolete, name="old.exe", pid=22,
            _guard=(guard, lambda: False),
        ))
        self.assertIs(selector.get_cached_gp(), current)
        self.assertEqual(obsolete.close_calls, 1)

    def test_dead_process_identity_invalidates_cached_reader(self):
        from gui_modules import sao_gui_process_selector as selector

        gp = self._GP(13)
        selector._cache_result(None, gp, name="a.exe", pid=13)
        gp.alive = False
        self.assertFalse(selector.get_cached_process_info()["attached"])
        self.assertEqual(gp.close_calls, 1)

    def test_mem_viewer_rejects_result_from_replaced_cache_generation(self):
        from gui_modules import sao_gui_process_selector as selector
        from mem_probe import mem_viewer

        old = self._GP(10)
        new = self._GP(20)
        selector._cache_result(None, old, name="a.exe", pid=10)
        old.on_read = lambda: selector._cache_result(None, new, name="b.exe", pid=20)

        result = mem_viewer.read_bytes("0x1000", 4)
        self.assertFalse(result["ok"])
        self.assertIn("changed", result["error"])


class PluginReaderRundownTests(unittest.TestCase):
    def test_unified_source_propagates_unconfirmed_bridge_rundown(self):
        from plugins.star_resonance_plugin.mem.unified_source import (
            UnifiedDataSource,
        )

        class _LiveBridge:
            stop_calls = 0

            def stop(self):
                self.stop_calls += 1
                return False

        source = UnifiedDataSource.__new__(UnifiedDataSource)
        source._bridge = _LiveBridge()
        source._started = True
        source._deferred = False
        source._last_error = ""
        source._notify_status = lambda *_args: None

        self.assertIs(source.stop(), False)
        self.assertTrue(source._started)
        self.assertEqual(source._bridge.stop_calls, 1)

    def test_poll_reader_does_not_close_process_until_worker_exits(self):
        from plugins.star_resonance_plugin.mem.reader import MemoryReader

        reader = MemoryReader(1, 2)
        release = threading.Event()

        class _PM:
            close_calls = 0

            def close(self):
                self.close_calls += 1

        pm = _PM()
        reader._pm = pm
        reader._thr = threading.Thread(target=lambda: release.wait(), daemon=True)
        reader._thr.start()

        reader.stop(join_timeout=0.01)
        self.assertEqual(pm.close_calls, 0)
        release.set()
        deadline = time.monotonic() + 1.0
        while pm.close_calls == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(pm.close_calls, 1)

    def test_static_source_defers_process_close_until_scan_exits(self):
        from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource

        source = StaticDpsSource()
        release = threading.Event()

        class _PM:
            close_calls = 0

            def close(self):
                self.close_calls += 1

        class _SR:
            pm = _PM()

        source._sr = _SR()
        source._scan_in_progress = True
        source._scan_thread = threading.Thread(target=lambda: release.wait(), daemon=True)
        source._scan_thread.start()

        self.assertFalse(source.close(wait_timeout=0.01))
        self.assertEqual(source._sr.pm.close_calls, 0)
        release.set()
        deadline = time.monotonic() + 1.0
        while source._sr is not None and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertIsNone(source._sr)
        self.assertEqual(_SR.pm.close_calls, 1)

    def test_provider_defers_source_close_until_poll_thread_exits(self):
        from plugins.star_resonance_plugin.mem.il2cpp.mem_self_state_provider import MemSelfStateProvider

        provider = MemSelfStateProvider()
        release = threading.Event()

        class _Source:
            close_calls = 0

            def close(self):
                self.close_calls += 1

        source = _Source()
        provider._src = source
        provider._thread = threading.Thread(target=lambda: release.wait(), daemon=True)
        provider._thread.start()

        self.assertFalse(provider.stop(join_timeout=0.01))
        self.assertEqual(source.close_calls, 0)
        release.set()
        deadline = time.monotonic() + 1.0
        while source.close_calls == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(source.close_calls, 1)

    def test_bridge_defers_provider_stop_until_shared_reader_threads_exit(self):
        from plugins.star_resonance_plugin.mem.il2cpp.mem_state_bridge import MemStateBridge

        bridge = MemStateBridge()
        release = threading.Event()

        class _Provider:
            stop_calls = 0

            def stop(self):
                self.stop_calls += 1

        provider = _Provider()
        bridge._provider = provider
        bridge._entity_thread = threading.Thread(target=lambda: release.wait(), daemon=True)
        bridge._entity_thread.start()

        self.assertFalse(bridge.stop(join_timeout=0.01))
        self.assertEqual(provider.stop_calls, 0)
        release.set()
        deadline = time.monotonic() + 1.0
        while provider.stop_calls == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(provider.stop_calls, 1)
        self.assertIsNone(bridge._provider)

    def test_manual_search_worker_is_cancelled_before_bridge_rundown(self):
        from plugins.star_resonance_plugin.mem.mem_access import (
            MemSearchManager, _SearchJob, cancel_search_jobs_for_bridge,
        )

        bridge = object()
        manager = MemSearchManager(object())
        job = _SearchJob("s1", "u32", 4, time.time())
        job._bridge = bridge
        job._thread = threading.Thread(target=lambda: job._stop.wait(), daemon=True)
        manager._jobs[job.job_id] = job
        job._thread.start()

        workers = cancel_search_jobs_for_bridge(bridge)
        self.assertEqual(workers, (job._thread,))
        self.assertTrue(job._stop.is_set())
        self.assertEqual(job.state, "cancelled")
        job._thread.join(timeout=1.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
