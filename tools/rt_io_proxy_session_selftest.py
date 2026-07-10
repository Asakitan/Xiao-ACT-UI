# -*- coding: utf-8 -*-
"""No-driver session/protocol tests for rt_io proxy and helper server.

All native process, pipe, crypto, and driver boundaries are replaced by fakes.
The suite never launches RuntimeBroker, opens a named pipe, or loads a driver.
"""
from __future__ import annotations

import atexit
import ctypes
import ctypes.wintypes as wt
import importlib.util
from pathlib import Path
import struct
import sys
import threading
import time
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MEM_PROBE = ROOT / "python" / "mem_probe"


def _load_source(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ATOMIC_NAME = "_rt_atomic_session_selftest"
PROXY_NAME = "_rt_proxy_session_selftest"
BACKEND_NAME = "_rt_backend_session_selftest"


def _atomic():
    module = sys.modules.get(ATOMIC_NAME)
    if module is None:
        module = _load_source(ATOMIC_NAME, MEM_PROBE / "_rt_atomic_globals.py")
    return module


def _proxy():
    module = sys.modules.get(PROXY_NAME)
    if module is None:
        module = _load_source(PROXY_NAME, MEM_PROBE / "rt_io_proxy.py")
    return module


def _backend():
    module = sys.modules.get(BACKEND_NAME)
    if module is None:
        module = _load_source(BACKEND_NAME, MEM_PROBE / "rt_io.py")
        atexit.unregister(module._z)
        atexit.unregister(module._helper_bcrypt_cleanup)
    return module


class ProtocolEnvelopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.wire = _atomic()

    def test_request_and_response_bind_request_id_deadline_and_outcome(self) -> None:
        request = self.wire.encode_ipc_request(
            request_id=0x100000001,
            deadline_ns=123456789,
            payload=b"abc",
            mutation=True,
        )
        decoded = self.wire.decode_ipc_request(request)
        self.assertEqual(decoded.request_id, 0x100000001)
        self.assertEqual(decoded.deadline_ns, 123456789)
        self.assertEqual(decoded.payload, b"abc")
        self.assertTrue(decoded.mutation)

        response = self.wire.encode_ipc_response(
            self.wire.WIRE_OUTCOME_COMMITTED,
            decoded.request_id,
            b"ok",
        )
        result = self.wire.decode_ipc_response(response)
        self.assertEqual(result.request_id, decoded.request_id)
        self.assertEqual(result.outcome, self.wire.WIRE_OUTCOME_COMMITTED)
        self.assertEqual(result.payload, b"ok")

    def test_envelope_rejects_truncation_trailing_bytes_and_oversize(self) -> None:
        good = self.wire.encode_ipc_request(1, 2, b"abc", False)
        for bad in (good[:-1], good + b"x"):
            with self.assertRaises(self.wire.ProtocolEnvelopeError):
                self.wire.decode_ipc_request(bad)
        with self.assertRaises(ValueError):
            self.wire.encode_ipc_request(
                1,
                2,
                b"x" * (self.wire.MAX_IPC_PAYLOAD + 1),
                False,
            )

    def test_command_schema_rejects_partial_batch_before_dispatch(self) -> None:
        valid = struct.pack("<I", 2)
        valid += struct.pack("<QI", 0x1000, 1)
        valid += struct.pack("<QI", 0x2000, 2)
        self.assertTrue(self.wire.validate_ipc_payload(0x1D, valid))
        self.assertFalse(self.wire.validate_ipc_payload(0x1D, valid[:-1]))
        too_many = struct.pack("<I", self.wire.MAX_BATCH_COUNT + 1)
        self.assertFalse(self.wire.validate_ipc_payload(0x1D, too_many))

        strict = struct.pack("<I", 1) + struct.pack(
            "<QII", 0x3000, 7, 8
        )
        self.assertTrue(self.wire.validate_ipc_payload(0x1E, strict))
        self.assertFalse(self.wire.validate_ipc_payload(0x1E, strict[:-1]))

    def test_read_schema_enforces_per_request_limit(self) -> None:
        valid = struct.pack("<QI", 0x1000, self.wire.MAX_READ_SIZE)
        invalid = struct.pack("<QI", 0x1000, self.wire.MAX_READ_SIZE + 1)
        self.assertTrue(self.wire.validate_ipc_payload(0x17, valid))
        self.assertFalse(self.wire.validate_ipc_payload(0x17, invalid))


class ProxyLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.proxy = _proxy()
        self.proxy._reset_session_state_for_tests()

    def tearDown(self) -> None:
        self.proxy._reset_session_state_for_tests()

    def test_state_and_outcome_enums_are_explicit(self) -> None:
        p = self.proxy
        self.assertEqual(
            [state.value for state in p.RtSessionState],
            ["STOPPED", "STARTING", "RUNNING", "QUIESCING", "CLEANING", "FAILED"],
        )
        self.assertEqual(
            [outcome.value for outcome in p.RequestOutcome],
            ["COMMITTED", "FAILED", "ROLLED_BACK", "UNKNOWN", "CANCELLED"],
        )

    def test_explicit_target_operation_blocks_legacy_pid_switch(self) -> None:
        p = self.proxy
        active = [0]

        def call(command, payload=b"", timeout=None):
            if command == p.CMD_ATTACH:
                active[0] = struct.unpack("<I", payload)[0]
                return b"\x01"
            if command == p.CMD_DETACH:
                active[0] = 0
                return b""
            if command == p.CMD_HL_READ:
                size = struct.unpack_from("<I", payload, 8)[0]
                return bytes((active[0] & 0xFF,)) * size
            raise AssertionError(f"unexpected command {command}")

        switched = threading.Event()
        with mock.patch.object(p, "_call", side_effect=call):
            self.assertTrue(p.attach(10))

            def legacy_switch():
                self.assertTrue(p.attach(30))
                switched.set()

            with p.target_operation(20):
                thread = threading.Thread(target=legacy_switch)
                thread.start()
                time.sleep(0.02)
                self.assertFalse(switched.is_set())
                self.assertEqual(p.read(0x1000, 4), b"\x14" * 4)
            thread.join(timeout=1)
            self.assertTrue(switched.is_set())
            self.assertEqual(p.read(0x1000, 4), b"\x1e" * 4)

    def test_explicit_release_restores_legacy_default_target(self) -> None:
        p = self.proxy
        active = [0]

        def call(command, payload=b"", timeout=None):
            if command == p.CMD_ATTACH:
                active[0] = struct.unpack("<I", payload)[0]
                return b"\x01"
            if command == p.CMD_DETACH:
                active[0] = 0
                return b""
            raise AssertionError(f"unexpected command {command}")

        with mock.patch.object(p, "_call", side_effect=call):
            self.assertTrue(p.attach(10))
            self.assertTrue(p._ensure_target(20))
            self.assertEqual(active[0], 20)
            self.assertTrue(p._release_target(20))
            self.assertEqual(active[0], 10)

    def test_concurrent_start_has_one_owner_and_one_epoch(self) -> None:
        p = self.proxy
        entered = threading.Event()
        release = threading.Event()
        launch_count = 0
        launch_lock = threading.Lock()

        def fake_launch(session, deadline):
            nonlocal launch_count
            with launch_lock:
                launch_count += 1
            entered.set()
            release.wait(1.0)
            return True

        results: list[bool] = []
        with mock.patch.object(p, "_launch_session", side_effect=fake_launch):
            first = threading.Thread(target=lambda: results.append(p._start_helper()))
            second = threading.Thread(target=lambda: results.append(p._start_helper()))
            first.start()
            self.assertTrue(entered.wait(1.0))
            second.start()
            time.sleep(0.02)
            release.set()
            first.join(1.0)
            second.join(1.0)

        self.assertEqual(launch_count, 1)
        self.assertEqual(results, [True, True])
        self.assertEqual(p._session_snapshot()["state"], "RUNNING")
        self.assertEqual(p._session_snapshot()["epoch"], 1)

    def test_popen_failure_does_not_poison_all_future_start_attempts(self) -> None:
        p = self.proxy
        launches = []

        def never_created(session, deadline):
            launches.append(session.epoch)
            return False

        with mock.patch.object(p, "_launch_session", side_effect=never_created):
            self.assertFalse(p._start_helper())
            first = p._current_session.last_cleanup
            self.assertEqual(p._session_snapshot()["state"], "STOPPED")
            self.assertIsNotNone(first)
            self.assertTrue(first.confirmed)
            self.assertFalse(first.ack_received)
            self.assertEqual(first.error, "no_session_created")
            stopped = p._stop_helper(wait_seconds=0.01)
            self.assertTrue(stopped.confirmed)
            self.assertFalse(stopped.ack_received)
            self.assertFalse(p._start_helper())

        self.assertEqual(launches, [1, 2])
        self.assertEqual(p._session_snapshot()["state"], "STOPPED")

    def test_partial_launch_cleanup_gets_a_fresh_deadline_budget(self) -> None:
        p = self.proxy

        class Proc:
            def poll(self):
                return None

        def partial_launch(session, deadline):
            session.proc = Proc()
            slot = p._PipeSlot(0, 909, session.epoch)
            session.slots[0] = slot
            session.pipe_pool.put(slot)
            return False

        report = p.CleanupReport(1, False, False, False, True, "test")
        with (
            mock.patch.object(p, "_launch_session", side_effect=partial_launch),
            mock.patch.object(p, "_remaining_seconds", return_value=0.0),
            mock.patch.object(p, "_stop_helper", return_value=report) as stop,
        ):
            self.assertFalse(p._start_helper())
        stop.assert_called_once_with(wait_seconds=15.0)

    def test_stop_waits_for_inflight_and_never_force_kills(self) -> None:
        p = self.proxy
        p._install_running_session_for_tests(epoch=7)
        lease = p._begin_request_for_tests()
        self.assertIsNotNone(lease)
        shutdown_called = threading.Event()

        def fake_shutdown(session, deadline):
            shutdown_called.set()
            return p.CleanupReport(
                epoch=session.epoch,
                confirmed=True,
                ack_received=True,
                child_exited=True,
                inflight_drained=True,
            )

        reports = []
        with mock.patch.object(p, "_shutdown_session", side_effect=fake_shutdown):
            stopper = threading.Thread(
                target=lambda: reports.append(p._stop_helper(wait_seconds=1.0))
            )
            stopper.start()
            time.sleep(0.03)
            self.assertFalse(shutdown_called.is_set())
            p._end_request_for_tests(lease)
            stopper.join(1.0)

        self.assertTrue(shutdown_called.is_set())
        self.assertTrue(reports[0])
        self.assertEqual(p._session_snapshot()["state"], "STOPPED")

    def test_expected_epoch_is_checked_atomically_before_inflight(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=7)
        self.assertIsNone(
            p._begin_request({p.RtSessionState.RUNNING}, expected_epoch=6))
        self.assertEqual(session.inflight, 0)
        lease = p._begin_request(
            {p.RtSessionState.RUNNING}, expected_epoch=7)
        self.assertIs(lease, session)
        self.assertEqual(session.inflight, 1)
        p._end_request(lease)

    def test_stop_sees_healthy_slots_only_after_inflight_disposition(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(
            epoch=18, handles=[1, 2, 3, 4]
        )
        entered = threading.Barrier(5)
        release = threading.Event()
        slots_seen_by_cleanup = []

        def roundtrip(_session, slot, command, payload,
                      request_id, deadline_ns):
            entered.wait(timeout=1.0)
            release.wait(1.0)
            return p.IpcCallResult(
                p.RequestOutcome.COMMITTED,
                payload=b"\x01",
                request_id=request_id,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        def shutdown(_session, deadline):
            slots_seen_by_cleanup.append(session.pipe_pool.qsize())
            return p.CleanupReport(18, True, True, True, True)

        results = []
        reports = []
        with (
            mock.patch.object(p, "_execute_roundtrip", side_effect=roundtrip),
            mock.patch.object(p, "_shutdown_session", side_effect=shutdown),
            mock.patch.object(p, "_close_pipe_handle"),
        ):
            callers = [
                threading.Thread(target=lambda: results.append(
                    p._call_result(p.CMD_PING, timeout=1.0)
                ))
                for _index in range(4)
            ]
            for caller in callers:
                caller.start()
            entered.wait(timeout=1.0)
            stopper = threading.Thread(
                target=lambda: reports.append(p._stop_helper(1.0))
            )
            stopper.start()
            deadline = time.monotonic() + 1.0
            while (
                p._session_snapshot()["state"] != "QUIESCING"
                and time.monotonic() < deadline
            ):
                time.sleep(0.005)
            release.set()
            for caller in callers:
                caller.join(2.0)
            stopper.join(2.0)

        self.assertTrue(all(not caller.is_alive() for caller in callers))
        self.assertFalse(stopper.is_alive())
        self.assertEqual(slots_seen_by_cleanup, [4])
        self.assertTrue(reports[0].confirmed)
        self.assertEqual(len(results), 4)
        self.assertTrue(all(result.reusable_transport for result in results))
        self.assertEqual(p._session_snapshot()["state"], "STOPPED")

    def test_start_stop_race_never_publishes_running(self) -> None:
        p = self.proxy
        entered = threading.Event()
        release = threading.Event()

        def fake_launch(session, deadline):
            entered.set()
            release.wait(1.0)
            return True

        def fake_shutdown(session, deadline):
            return p.CleanupReport(
                session.epoch, True, True, True, True
            )

        starts = []
        stops = []
        with (
            mock.patch.object(p, "_launch_session", side_effect=fake_launch),
            mock.patch.object(p, "_shutdown_session", side_effect=fake_shutdown),
        ):
            starter = threading.Thread(
                target=lambda: starts.append(p._start_helper())
            )
            stopper = threading.Thread(
                target=lambda: stops.append(p._stop_helper(1.0))
            )
            starter.start()
            self.assertTrue(entered.wait(1.0))
            stopper.start()
            time.sleep(0.03)
            release.set()
            starter.join(1.0)
            stopper.join(1.0)

        self.assertEqual(starts, [False])
        self.assertTrue(stops[0])
        self.assertEqual(p._session_snapshot()["state"], "STOPPED")
        self.assertFalse(p._connected)

    def test_stop_timeout_cannot_orphan_a_late_successful_launch(self) -> None:
        p = self.proxy
        entered = threading.Event()
        release = threading.Event()
        cleaned = threading.Event()

        class Proc:
            def poll(self):
                return None

        def late_launch(session, deadline):
            entered.set()
            release.wait(1.0)
            session.proc = Proc()
            slot = p._PipeSlot(0, 808, session.epoch)
            session.slots[0] = slot
            session.pipe_pool.put(slot)
            return True

        def confirmed_cleanup(session, deadline):
            cleaned.set()
            return p.CleanupReport(
                session.epoch, True, True, True, True
            )

        starts = []
        stops = []
        with (
            mock.patch.object(p, "_launch_session", side_effect=late_launch),
            mock.patch.object(
                p, "_shutdown_session", side_effect=confirmed_cleanup
            ),
            mock.patch.object(p, "_close_pipe_handle"),
        ):
            starter = threading.Thread(
                target=lambda: starts.append(p._start_helper())
            )
            starter.start()
            self.assertTrue(entered.wait(1.0))
            stopper = threading.Thread(
                target=lambda: stops.append(p._stop_helper(0.02))
            )
            stopper.start()
            stopper.join(1.0)
            self.assertFalse(stops[0].confirmed)
            self.assertEqual(stops[0].error, "start_stop_timeout")
            release.set()
            starter.join(2.0)

        self.assertFalse(starter.is_alive())
        self.assertEqual(starts, [False])
        self.assertTrue(cleaned.is_set())
        self.assertEqual(p._session_snapshot()["state"], "STOPPED")
        self.assertFalse(p._connected)

    def test_timed_out_mutation_is_unknown_and_not_retried_or_requeued(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=9, handles=[123])
        calls = 0

        def blocked_roundtrip(*_args, **_kwargs):
            nonlocal calls
            calls += 1
            time.sleep(0.05)
            return p.IpcCallResult(
                outcome=p.RequestOutcome.COMMITTED,
                payload=b"\x01",
                request_id=1,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        with (
            mock.patch.object(p, "_execute_roundtrip", side_effect=blocked_roundtrip),
            mock.patch.object(p, "_schedule_slot_replacement"),
            mock.patch.object(p, "_cancel_synchronous_io") as cancel_sync,
        ):
            result = p._call_result(p.CMD_PW, struct.pack("<QI", 1, 2), timeout=0.01)

        self.assertEqual(result.outcome, p.RequestOutcome.UNKNOWN)
        self.assertEqual(calls, 1)
        self.assertEqual(session.pipe_pool.qsize(), 0)
        self.assertEqual(cancel_sync.call_count, 1)
        self.assertGreater(cancel_sync.call_args.args[0], 0)

    def test_pretransport_cancel_is_cancelled_even_for_mutation(self) -> None:
        p = self.proxy
        result = p._cancelled_before_transport(123)
        self.assertEqual(result.outcome, p.RequestOutcome.CANCELLED)
        self.assertEqual(result.request_id, 123)
        self.assertEqual(result.error, "cancelled_before_transport")

    def test_real_timeout_before_worker_gate_is_cancelled_not_unknown(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=20, handles=[77])
        deferred = []

        class DeferredThread:
            def __init__(self, target=None, **_kwargs):
                self.target = target

            def start(self):
                deferred.append(self.target)

        with (
            mock.patch.object(p.threading, "Thread", DeferredThread),
            mock.patch.object(p, "_schedule_slot_replacement"),
            mock.patch.object(p, "_cancel_synchronous_io"),
            mock.patch.object(p, "_request_pipe_cancel"),
            mock.patch.object(p, "_close_pipe_handle"),
            mock.patch.object(
                p, "_execute_roundtrip",
                side_effect=AssertionError("transport must not begin"),
            ),
        ):
            result = p._call_result(
                p.CMD_PW, struct.pack("<QI", 1, 2), timeout=0.01)
            self.assertEqual(result.outcome, p.RequestOutcome.CANCELLED)
            self.assertEqual(result.error, "cancelled_before_transport")
            self.assertEqual(len(deferred), 1)
            deferred.pop()()
        self.assertEqual(session.inflight, 0)

    def test_timeout_does_not_close_handle_until_worker_returns(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=19, handles=[321])
        entered = threading.Event()
        release = threading.Event()
        closed = []

        def blocked(_session, slot, command, payload,
                    request_id, deadline_ns):
            entered.set()
            release.wait(1.0)
            self.assertFalse(slot.closed)
            return p.IpcCallResult(
                p.RequestOutcome.COMMITTED,
                payload=b"\x01",
                request_id=request_id,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        def close(slot):
            slot.closed = True
            closed.append(slot.handle)

        with (
            mock.patch.object(p, "_execute_roundtrip", side_effect=blocked),
            mock.patch.object(p, "_close_pipe_handle", side_effect=close),
            mock.patch.object(p, "_request_pipe_cancel"),
            mock.patch.object(p, "_cancel_synchronous_io"),
            mock.patch.object(p, "_schedule_slot_replacement"),
        ):
            result = p._call_result(p.CMD_PING, timeout=0.01)
            self.assertTrue(entered.is_set())
            self.assertEqual(result.outcome, p.RequestOutcome.CANCELLED)
            self.assertEqual(closed, [])
            self.assertEqual(session.slots, {})
            release.set()
            deadline = time.monotonic() + 1.0
            while session.inflight and time.monotonic() < deadline:
                time.sleep(0.005)

        self.assertEqual(session.inflight, 0)
        self.assertEqual(closed, [321])

    def test_four_pipe_roundtrips_can_progress_concurrently(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(
            epoch=10, handles=[1, 2, 3, 4]
        )
        barrier = threading.Barrier(4)
        active = 0
        max_active = 0
        active_lock = threading.Lock()

        def concurrent(session, slot, command, payload,
                       request_id, deadline_ns):
            nonlocal active, max_active
            with active_lock:
                active += 1
                max_active = max(max_active, active)
            barrier.wait(timeout=1.0)
            with active_lock:
                active -= 1
            return p.IpcCallResult(
                p.RequestOutcome.COMMITTED,
                payload=b"\x01",
                request_id=request_id,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        results = []
        with mock.patch.object(p, "_execute_roundtrip", side_effect=concurrent):
            workers = [
                threading.Thread(
                    target=lambda: results.append(
                        p._call_result(p.CMD_PING, timeout=1.0)
                    )
                )
                for _index in range(4)
            ]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(2.0)

        self.assertTrue(all(not worker.is_alive() for worker in workers))
        self.assertEqual(max_active, 4)
        self.assertEqual(len(results), 4)
        self.assertTrue(all(
            result.outcome == p.RequestOutcome.COMMITTED
            for result in results
        ))
        self.assertEqual(session.pipe_pool.qsize(), 4)

    def test_consecutive_timeouts_replace_slots_and_later_recover(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=12, handles=[1])
        calls = 0
        replacements = 0

        def roundtrip(_session, slot, command, payload,
                      request_id, deadline_ns):
            nonlocal calls
            calls += 1
            if calls <= 2:
                time.sleep(0.03)
            return p.IpcCallResult(
                p.RequestOutcome.COMMITTED,
                payload=b"\x01",
                request_id=request_id,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        def replace(_session, slot_index):
            nonlocal replacements
            replacements += 1
            slot = p._PipeSlot(slot_index, 100 + replacements, session.epoch)
            session.slots[slot_index] = slot
            session.pipe_pool.put(slot)

        def close(slot):
            slot.closed = True

        with (
            mock.patch.object(p, "_execute_roundtrip", side_effect=roundtrip),
            mock.patch.object(p, "_schedule_slot_replacement", side_effect=replace),
            mock.patch.object(p, "_close_pipe_handle", side_effect=close),
        ):
            first = p._call_result(p.CMD_PING, timeout=0.005)
            second = p._call_result(p.CMD_PING, timeout=0.005)
            third = p._call_result(p.CMD_PING, timeout=0.2)
            deadline = time.monotonic() + 1.0
            while session.inflight and time.monotonic() < deadline:
                time.sleep(0.005)

        self.assertEqual(first.outcome, p.RequestOutcome.CANCELLED)
        self.assertEqual(second.outcome, p.RequestOutcome.CANCELLED)
        self.assertEqual(third.outcome, p.RequestOutcome.COMMITTED)
        self.assertEqual(replacements, 2)
        self.assertEqual(calls, 3)
        self.assertEqual(session.inflight, 0)
        self.assertEqual(session.pipe_pool.qsize(), 1)

    def test_only_fully_matched_authenticated_response_returns_slot(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=3, handles=[55])

        def successful(session, slot, command, payload, request_id, deadline_ns):
            return p.IpcCallResult(
                p.RequestOutcome.COMMITTED,
                payload=b"ok",
                request_id=request_id,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        with mock.patch.object(p, "_execute_roundtrip", side_effect=successful):
            result = p._call_result(p.CMD_PING, timeout=0.1)
        self.assertEqual(result.payload, b"ok")
        self.assertEqual(session.pipe_pool.qsize(), 1)

        def mismatched(session, slot, command, payload, request_id, deadline_ns):
            return p.IpcCallResult(
                p.RequestOutcome.COMMITTED,
                payload=b"wrong",
                request_id=request_id,
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=False,
            )

        with (
            mock.patch.object(p, "_execute_roundtrip", side_effect=mismatched),
            mock.patch.object(p, "_close_pipe_handle"),
            mock.patch.object(p, "_schedule_slot_replacement"),
        ):
            mismatch = p._call_result(p.CMD_PING, timeout=0.1)
        self.assertEqual(mismatch.outcome, p.RequestOutcome.COMMITTED)
        self.assertFalse(mismatch.request_id_matched)
        self.assertEqual(session.pipe_pool.qsize(), 0)

        with mock.patch.object(p, "_call_result", return_value=mismatch):
            self.assertIsNone(p._call(p.CMD_PING))

    def test_missing_pipe_slot_retries_until_recreated(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=6)
        attempts = []

        def connect(*args):
            attempts.append(args)
            return 77 if len(attempts) >= 3 else None

        with mock.patch.object(p, "_connect_pipe_handle", side_effect=connect):
            p._schedule_slot_replacement(session, 2)
            deadline = time.monotonic() + 1.0
            while session.pipe_pool.qsize() == 0 and time.monotonic() < deadline:
                time.sleep(0.01)
        self.assertGreaterEqual(len(attempts), 3)
        self.assertEqual(session.pipe_pool.qsize(), 1)
        slot = session.pipe_pool.get_nowait()
        self.assertEqual((slot.index, slot.handle), (2, 77))

    def test_pipe_reconnect_budget_marks_session_unhealthy(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(epoch=16)
        cleanup_started = threading.Event()

        def cleanup(*args, **kwargs):
            cleanup_started.set()
            return p.CleanupReport(16, False, False, False, True, "test")

        with (
            mock.patch.object(p, "_PIPE_RECONNECT_BUDGET", 0.02),
            mock.patch.object(p, "_connect_pipe_handle", return_value=None),
            mock.patch.object(p, "_stop_helper", side_effect=cleanup),
        ):
            p._schedule_slot_replacement(session, 0)
            deadline = time.monotonic() + 1.0
            while (
                p._session_snapshot()["state"] != "FAILED"
                and time.monotonic() < deadline
            ):
                time.sleep(0.005)
            self.assertTrue(cleanup_started.wait(1.0))

        self.assertEqual(p._session_snapshot()["state"], "FAILED")
        self.assertFalse(p._connected)

    def test_cleanup_report_requires_ack_and_child_exit(self) -> None:
        p = self.proxy
        report = p.CleanupReport(
            epoch=1,
            confirmed=False,
            ack_received=True,
            child_exited=False,
            inflight_drained=True,
            error="child_timeout",
        )
        self.assertFalse(report)
        self.assertFalse(report.confirmed)

    def test_child_timeout_returns_unconfirmed_without_kill(self) -> None:
        p = self.proxy

        class FakeProcess:
            def __init__(self):
                self.kill_calls = 0

            def poll(self):
                return None

            def wait(self, timeout=None):
                raise TimeoutError("still cleaning")

            def kill(self):
                self.kill_calls += 1

        process = FakeProcess()
        session = p._install_running_session_for_tests(epoch=4, handles=[88])
        session.proc = process
        ack = p.IpcCallResult(
            p.RequestOutcome.COMMITTED,
            payload=b"\x01",
            request_id=1,
            authenticated=True,
            transport_complete=True,
            seq_matched=True,
            request_id_matched=True,
        )
        with (
            mock.patch.object(p, "_call_result", return_value=ack),
            mock.patch.object(p, "_close_pipe_handle"),
        ):
            report = p._stop_helper(wait_seconds=0.01)
        self.assertFalse(report)
        self.assertTrue(report.ack_received)
        self.assertFalse(report.child_exited)
        self.assertEqual(process.kill_calls, 0)
        self.assertEqual(p._session_snapshot()["state"], "FAILED")

    def test_cleanup_ack_plus_child_exit_is_confirmed(self) -> None:
        p = self.proxy

        class ExitingProcess:
            def __init__(self):
                self.exited = False

            def poll(self):
                return 0 if self.exited else None

            def wait(self, timeout=None):
                self.exited = True
                return 0

        session = p._install_running_session_for_tests(epoch=5, handles=[89])
        session.proc = ExitingProcess()
        ack = p.IpcCallResult(
            p.RequestOutcome.COMMITTED,
            payload=b"\x01",
            request_id=1,
            authenticated=True,
            transport_complete=True,
            seq_matched=True,
            request_id_matched=True,
        )
        with (
            mock.patch.object(p, "_call_result", return_value=ack),
            mock.patch.object(p, "_close_pipe_handle"),
        ):
            report = p._stop_helper(wait_seconds=0.2)
        self.assertTrue(report)
        self.assertTrue(report.ack_received)
        self.assertTrue(report.child_exited)
        self.assertEqual(p._session_snapshot()["state"], "STOPPED")

    def test_unconfirmed_child_exit_still_closes_local_pipe_handles(self) -> None:
        p = self.proxy

        class ExitedProcess:
            def poll(self):
                return 0

        session = p._install_running_session_for_tests(epoch=15, handles=[90])
        session.proc = ExitedProcess()
        unknown = p.IpcCallResult(
            p.RequestOutcome.UNKNOWN,
            request_id=1,
            error="cleanup_unknown",
        )
        closed = []
        with (
            mock.patch.object(p, "_call_result", return_value=unknown),
            mock.patch.object(
                p, "_close_pipe_handle",
                side_effect=lambda slot: closed.append(slot.handle),
            ),
        ):
            report = p._stop_helper(wait_seconds=0.2)
        self.assertFalse(report.confirmed)
        self.assertTrue(report.child_exited)
        self.assertEqual(closed, [90])
        self.assertEqual(session.slots, {})
        self.assertEqual(p._session_snapshot()["state"], "FAILED")

    def test_epoch_change_invalidates_hmv_cache_and_is_visible_in_status(self) -> None:
        p = self.proxy
        p._hmv_cache = 0x1234
        p._ipc_aad_cache = b"stale"
        p._install_running_session_for_tests(epoch=11)
        self.assertIsNone(p._hmv_cache)
        self.assertIsNone(p._ipc_aad_cache)
        with mock.patch.object(p, "_call", return_value=None):
            status = p.status()
        self.assertEqual(status["session_epoch"], 11)

    def test_nonce_and_sequence_never_wrap(self) -> None:
        p = self.proxy
        p._nonce_ctr = 0xFFFFFFFF
        with self.assertRaises(p.SessionRekeyRequired):
            p._next_nonce()
        p._seq = 0xFFFFFFFF
        with self.assertRaises(p.SessionRekeyRequired):
            p._next_seq()

    def test_counter_watermark_rotates_before_consuming_a_pipe(self) -> None:
        p = self.proxy
        session = p._install_running_session_for_tests(
            epoch=14, handles=[1234]
        )
        p._nonce_ctr = p._REKEY_WATERMARK
        with mock.patch.object(p, "_schedule_session_rekey") as schedule:
            result = p._call_result(p.CMD_PING, timeout=0.01)

        self.assertEqual(result.outcome, p.RequestOutcome.CANCELLED)
        self.assertEqual(result.error, "rekey_required")
        schedule.assert_called_once_with(session)
        self.assertEqual(session.pipe_pool.qsize(), 1)
        self.assertEqual(session.inflight, 0)

    def test_large_read_is_chunked_without_changing_return_shape(self) -> None:
        p = self.proxy
        p._attached_pid = p._default_target_pid = 99
        calls = []

        def fake_call(command, payload=b"", timeout=None):
            address, size = struct.unpack("<QI", payload)
            calls.append((command, address, size))
            return bytes([address & 0xFF]) * size

        with (
            mock.patch.object(p._wire, "MAX_READ_SIZE", 4),
            mock.patch.object(p, "_call", side_effect=fake_call),
        ):
            data = p.read(0x1000, 10)
        self.assertEqual(len(data), 10)
        self.assertEqual(calls, [
            (p.CMD_HL_READ, 0x1000, 4),
            (p.CMD_HL_READ, 0x1004, 4),
            (p.CMD_HL_READ, 0x1008, 2),
        ])

    def test_read_batch_is_partitioned_by_count_and_total_bytes(self) -> None:
        p = self.proxy
        p._attached_pid = p._default_target_pid = 99
        groups = []

        def fake_call(command, payload=b"", timeout=None):
            count = struct.unpack_from("<I", payload, 0)[0]
            groups.append(count)
            response = []
            offset = 4
            for _ in range(count):
                address, size = struct.unpack_from("<QI", payload, offset)
                offset += 12
                response.append(struct.pack("<BI", 1, size))
                response.append(bytes([address & 0xFF]) * size)
            return b"".join(response)

        requests = [(0x1000 + index, 1) for index in range(5)]
        with (
            mock.patch.object(p._wire, "MAX_BATCH_COUNT", 2),
            mock.patch.object(p._wire, "MAX_BATCH_BYTES", 2),
            mock.patch.object(p, "_call", side_effect=fake_call),
        ):
            results = p.read_batch(requests)
        self.assertEqual(groups, [2, 2, 1])
        self.assertEqual(results, [b"\x00", b"\x01", b"\x02", b"\x03", b"\x04"])


class TransportBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.proxy = _proxy()

    def test_write_all_honors_native_partial_byte_counts(self) -> None:
        p = self.proxy
        sizes = []

        def write_file(handle, pointer, size, written, overlapped):
            count = min(2, int(size))
            sizes.append(count)
            ctypes.cast(written, ctypes.POINTER(wt.DWORD))[0] = count
            return True

        with mock.patch.object(p._k32, "WriteFile", side_effect=write_file):
            self.assertTrue(p._write_all(1, b"abcde"))
        self.assertEqual(sizes, [2, 2, 1])

    def test_concurrent_slot_disposal_closes_native_handle_once(self) -> None:
        p = self.proxy
        slot = p._PipeSlot(0, 4321, 1)
        cancelled = []
        closed = []

        def cancel(handle, overlapped):
            cancelled.append(handle)
            time.sleep(0.005)
            return True

        def close(handle):
            closed.append(handle)
            return True

        with (
            mock.patch.object(p._k32, "CancelIoEx", side_effect=cancel),
            mock.patch.object(p._k32, "CloseHandle", side_effect=close),
        ):
            workers = [
                threading.Thread(target=lambda: p._close_pipe_handle(slot))
                for _index in range(8)
            ]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(1.0)
        self.assertTrue(slot.closed)
        self.assertEqual(cancelled, [4321])
        self.assertEqual(closed, [4321])

    def test_read_exact_honors_native_partial_byte_counts(self) -> None:
        p = self.proxy
        source = bytearray(b"abcde")
        offset = 0

        def read_file(handle, pointer, size, received, overlapped):
            nonlocal offset
            count = min(2, int(size), len(source) - offset)
            if count <= 0:
                return False
            ctypes.memmove(pointer, bytes(source[offset:offset + count]), count)
            ctypes.cast(received, ctypes.POINTER(wt.DWORD))[0] = count
            offset += count
            return True

        with mock.patch.object(p._k32, "ReadFile", side_effect=read_file):
            self.assertEqual(p._read_exact(1, 5), b"abcde")

    def test_roundtrip_rejects_broken_write_and_short_response(self) -> None:
        p = self.proxy
        session = p._RtSession(
            epoch=1,
            state=p.RtSessionState.RUNNING,
            wire_key=b"k" * 32,
        )
        slot = p._PipeSlot(0, 1, 1)
        with mock.patch.object(p, "_pipe_send_on", return_value=False):
            broken = p._execute_roundtrip(
                session, slot, p.CMD_PING, b"", 1, 100
            )
        self.assertEqual(broken.error, "short_write")
        self.assertFalse(broken.reusable_transport)

        with (
            mock.patch.object(p, "_pipe_send_on", return_value=True),
            mock.patch.object(p, "_pipe_recv_on", return_value=b"1234"),
        ):
            short = p._execute_roundtrip(
                session, slot, p.CMD_PING, b"", 2, 100
            )
        self.assertEqual(short.error, "short_response")
        self.assertFalse(short.reusable_transport)

    def test_lost_mutation_response_is_unknown_not_retryable_failure(self) -> None:
        p = self.proxy
        session = p._RtSession(
            epoch=1,
            state=p.RtSessionState.RUNNING,
            wire_key=b"k" * 32,
        )
        slot = p._PipeSlot(0, 1, 1)
        with (
            mock.patch.object(p, "_pipe_send_on", return_value=True),
            mock.patch.object(p, "_pipe_recv_on", return_value=None),
        ):
            result = p._execute_roundtrip(
                session,
                slot,
                p.CMD_PW,
                struct.pack("<QI", 0x1000, 1),
                5,
                100,
            )
        self.assertEqual(result.outcome, p.RequestOutcome.UNKNOWN)
        self.assertEqual(
            result.error, "unauthenticated_or_truncated_response"
        )
        self.assertFalse(result.reusable_transport)

    def test_wrong_request_id_cannot_return_another_requests_payload(self) -> None:
        p = self.proxy
        session = p._RtSession(
            epoch=1,
            state=p.RtSessionState.RUNNING,
            wire_key=b"k" * 32,
        )
        slot = p._PipeSlot(0, 1, 1)
        wrong = p._wire.encode_ipc_response(
            p._wire.WIRE_OUTCOME_COMMITTED,
            999,
            b"wrong",
        )
        outer = struct.pack("<BI", 0, 7) + wrong
        with (
            mock.patch.object(p, "_next_seq", return_value=7),
            mock.patch.object(p, "_pipe_send_on", return_value=True),
            mock.patch.object(p, "_pipe_recv_on", return_value=outer),
        ):
            read_result = p._execute_roundtrip(
                session, slot, p.CMD_PING, b"", 5, 100
            )
            write_result = p._execute_roundtrip(
                session,
                slot,
                p.CMD_PW,
                struct.pack("<QI", 0x1000, 1),
                6,
                100,
            )
        self.assertEqual(read_result.outcome, p.RequestOutcome.FAILED)
        self.assertEqual(write_result.outcome, p.RequestOutcome.UNKNOWN)
        self.assertEqual(read_result.error, "request_id_mismatch")
        self.assertEqual(write_result.error, "request_id_mismatch")
        self.assertFalse(read_result.reusable_transport)
        self.assertFalse(write_result.reusable_transport)

    def test_pipe_receive_rejects_decryption_failure(self) -> None:
        p = self.proxy
        encrypted = b"x" * 28
        with (
            mock.patch.object(
                p, "_read_exact",
                side_effect=[struct.pack("<I", len(encrypted)), encrypted],
            ),
            mock.patch.object(p, "_aes_decrypt", return_value=None),
        ):
            self.assertIsNone(p._pipe_recv_on(1, b"k" * 32))

    def test_cng_initialization_failure_is_not_published_as_bound(self) -> None:
        p = self.proxy

        class FakeBcrypt:
            def BCryptOpenAlgorithmProvider(self, *args):
                return -1

        old = (p._bcrypt, p._key_bound)
        try:
            p._bcrypt = FakeBcrypt()
            p._key_bound = None
            with self.assertRaises(RuntimeError):
                p._ensure_bcrypt(b"k" * 32)
            self.assertIsNone(p._key_bound)
        finally:
            p._bcrypt, p._key_bound = old

    def test_helper_cng_failure_is_not_published_as_bound(self) -> None:
        rt = _backend()

        class FakeBcrypt:
            def BCryptOpenAlgorithmProvider(self, *args):
                return -1

        old = (rt._helper_bcrypt, rt._helper_key_bound)
        try:
            rt._helper_bcrypt = FakeBcrypt()
            rt._helper_key_bound = None
            with self.assertRaises(RuntimeError):
                rt._ensure_helper_bcrypt(b"k" * 32)
            self.assertIsNone(rt._helper_key_bound)
        finally:
            rt._helper_bcrypt, rt._helper_key_bound = old

    def test_cng_rotation_refuses_to_replace_an_unclean_old_key(self) -> None:
        class FailedOldKeyCleanup:
            def __init__(self):
                self.open_calls = 0

            def BCryptDestroyKey(self, *args):
                return 0xC0000001

            def BCryptCloseAlgorithmProvider(self, *args):
                return 0

            def BCryptOpenAlgorithmProvider(self, *args):
                self.open_calls += 1
                return 0

            def BCryptSetProperty(self, *args):
                return 0

            def BCryptGenerateSymmetricKey(self, *args):
                return 0

        rt = _backend()
        cases = (
            (
                "proxy",
                self.proxy,
                "_bcrypt",
                "_key_bound",
                self.proxy._ensure_bcrypt,
            ),
            (
                "helper",
                rt,
                "_helper_bcrypt",
                "_helper_key_bound",
                rt._ensure_helper_bcrypt,
            ),
        )
        for label, module, bcrypt_name, bound_name, ensure in cases:
            with self.subTest(side=label):
                fake = FailedOldKeyCleanup()
                with (
                    mock.patch.object(module, bcrypt_name, fake),
                    mock.patch.object(module, bound_name, b"o" * 32),
                ):
                    with self.assertRaises(RuntimeError):
                        ensure(b"n" * 32)
                    self.assertEqual(getattr(module, bound_name), b"o" * 32)
                    self.assertEqual(fake.open_calls, 0)

    def test_decrypt_rejects_native_short_plaintext_count(self) -> None:
        p = self.proxy

        class ShortDecrypt:
            def BCryptDecrypt(self, key, ciphertext, size, auth, iv,
                              iv_size, plaintext, capacity, written, flags):
                ctypes.cast(written, ctypes.POINTER(ctypes.c_ulong))[0] = (
                    int(size) - 1
                )
                return 0

        old = p._bcrypt
        try:
            p._bcrypt = ShortDecrypt()
            with (
                mock.patch.object(p, "_ensure_bcrypt"),
                mock.patch.object(p, "_ipc_aad", return_value=b""),
            ):
                encrypted = b"n" * 12 + b"abc" + b"t" * 16
                self.assertIsNone(p._aes_decrypt(b"k" * 32, encrypted))
        finally:
            p._bcrypt = old

    def test_helper_decrypt_rejects_native_short_plaintext_count(self) -> None:
        rt = _backend()

        class ShortDecrypt:
            def BCryptDecrypt(self, key, ciphertext, size, auth, iv,
                              iv_size, plaintext, capacity, written, flags):
                ctypes.cast(written, ctypes.POINTER(ctypes.c_ulong))[0] = (
                    int(size) - 1
                )
                return 0

        old = rt._helper_bcrypt
        try:
            rt._helper_bcrypt = ShortDecrypt()
            with (
                mock.patch.object(rt, "_ensure_helper_bcrypt"),
                mock.patch.object(rt, "_helper_aad", return_value=b""),
            ):
                encrypted = b"n" * 12 + b"abc" + b"t" * 16
                self.assertIsNone(
                    rt._helper_aes_decrypt(b"k" * 32, encrypted)
                )
        finally:
            rt._helper_bcrypt = old

    def test_helper_nonce_never_wraps(self) -> None:
        rt = _backend()
        old = rt._helper_nonce_ctr
        try:
            rt._helper_nonce_ctr = 0xFFFFFFFF
            with self.assertRaises(RuntimeError):
                rt._helper_next_nonce()
        finally:
            rt._helper_nonce_ctr = old


class HelperExecutorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.rt = _backend()
        self.wire = _atomic()

    def _request(self, request_id, payload=b"", mutation=False, deadline=10_000):
        return self.wire.encode_ipc_request(
            request_id, deadline, payload, mutation
        )

    def test_helper_cleanup_ack_uses_structured_backend_report(self) -> None:
        with (
            mock.patch.object(
                self.rt, "_z", return_value={"confirmed": False}
            ),
            mock.patch.object(self.rt.atexit, "unregister") as unregister,
        ):
            self.assertFalse(self.rt._helper_cleanup_backend())
            unregister.assert_not_called()
        with (
            mock.patch.object(
                self.rt, "_z", return_value={"confirmed": True}
            ),
            mock.patch.object(self.rt.atexit, "unregister") as unregister,
        ):
            self.assertTrue(self.rt._helper_cleanup_backend())
            unregister.assert_called_once_with(self.rt._z)

    def test_expired_request_is_cancelled_before_dispatch(self) -> None:
        dispatched = []

        def dispatch(*args):
            dispatched.append(args)
            raise AssertionError("expired request must not dispatch")

        executor = self.rt._HelperRequestExecutor(
            dispatch=dispatch,
            cleanup=lambda: True,
            now_ns=lambda: 20_000,
        )
        event = threading.Event()
        execution = executor.execute(
            self.rt._HELPER_CMD_ATTACH,
            1,
            self._request(7, struct.pack("<I", 100), True),
            event,
        )
        response = self.wire.decode_ipc_response(execution.response)
        self.assertEqual(response.outcome, self.wire.WIRE_OUTCOME_CANCELLED)
        self.assertEqual(dispatched, [])

    def test_mutation_request_id_is_deduplicated(self) -> None:
        dispatch_count = 0

        def dispatch(cmd, seq, payload, send, event):
            nonlocal dispatch_count
            dispatch_count += 1
            send(0, seq, b"\x01")
            return True

        executor = self.rt._HelperRequestExecutor(
            dispatch=dispatch,
            cleanup=lambda: True,
            now_ns=lambda: 1,
        )
        envelope = self._request(
            99, struct.pack("<QI", 0x1000, 1), True, deadline=100
        )
        first = executor.execute(self.rt._HELPER_CMD_PW, 1, envelope, threading.Event())
        second = executor.execute(self.rt._HELPER_CMD_PW, 2, envelope, threading.Event())
        self.assertEqual(dispatch_count, 1)
        self.assertEqual(first.response, second.response)

    def test_same_request_id_with_different_payload_is_rejected(self) -> None:
        def dispatch(cmd, seq, payload, send, event):
            send(0, seq, b"\x01")
            return True

        executor = self.rt._HelperRequestExecutor(
            dispatch=dispatch,
            cleanup=lambda: True,
            now_ns=lambda: 1,
        )
        first = self._request(5, struct.pack("<QI", 1, 1), True, 100)
        second = self._request(5, struct.pack("<QI", 2, 2), True, 100)
        executor.execute(self.rt._HELPER_CMD_PW, 1, first, threading.Event())
        result = executor.execute(
            self.rt._HELPER_CMD_PW, 2, second, threading.Event()
        )
        decoded = self.wire.decode_ipc_response(result.response)
        self.assertEqual(decoded.outcome, self.wire.WIRE_OUTCOME_FAILED)

    def test_dispatch_exception_after_mutation_start_is_cached_unknown(self) -> None:
        dispatch_count = 0
        effects = []

        def partial_dispatch(cmd, seq, payload, send, event):
            nonlocal dispatch_count
            dispatch_count += 1
            effects.append(payload)
            raise RuntimeError("response lost after mutation")

        executor = self.rt._HelperRequestExecutor(
            dispatch=partial_dispatch,
            cleanup=lambda: True,
            now_ns=lambda: 1,
        )
        envelope = self._request(
            55, struct.pack("<QI", 0x1000, 1), True, 100
        )
        first = executor.execute(
            self.rt._HELPER_CMD_PW, 1, envelope, threading.Event()
        )
        second = executor.execute(
            self.rt._HELPER_CMD_PW, 2, envelope, threading.Event()
        )
        decoded = self.wire.decode_ipc_response(first.response)
        self.assertEqual(decoded.outcome, self.wire.WIRE_OUTCOME_UNKNOWN)
        self.assertEqual(first.response, second.response)
        self.assertEqual(dispatch_count, 1)
        self.assertEqual(len(effects), 1)

    def test_oversize_dispatch_response_is_bounded_before_encoding(self) -> None:
        wire = self.wire
        executor_wire = self.rt._helper_wire_protocol()

        def oversize(cmd, seq, payload, send, event):
            send(0, seq, b"x" * 9)
            return True

        executor = self.rt._HelperRequestExecutor(
            dispatch=oversize,
            cleanup=lambda: True,
            now_ns=lambda: 1,
        )
        read_envelope = self._request(61, b"", False, 100)
        mutation_envelope = self._request(62, b"", True, 100)
        with mock.patch.object(executor_wire, "MAX_IPC_PAYLOAD", 8):
            read_result = executor.execute(
                self.rt._HELPER_CMD_PING,
                1,
                read_envelope,
                threading.Event(),
            )
            mutation_result = executor.execute(
                self.rt._HELPER_CMD_DETACH,
                2,
                mutation_envelope,
                threading.Event(),
            )
        read_response = wire.decode_ipc_response(read_result.response)
        mutation_response = wire.decode_ipc_response(mutation_result.response)
        self.assertEqual(read_response.outcome, wire.WIRE_OUTCOME_FAILED)
        self.assertEqual(mutation_response.outcome, wire.WIRE_OUTCOME_UNKNOWN)
        self.assertEqual(read_response.payload, b"")
        self.assertEqual(mutation_response.payload, b"")

    def test_shutdown_ack_is_committed_only_after_confirmed_cleanup(self) -> None:
        events = []
        shutdown = threading.Event()
        executor = self.rt._HelperRequestExecutor(
            dispatch=lambda *_args: (_ for _ in ()).throw(
                AssertionError("shutdown bypasses normal dispatch")
            ),
            cleanup=lambda: events.append("cleanup") or True,
            now_ns=lambda: 1,
        )
        execution = executor.execute(
            self.rt._HELPER_CMD_SHUTDOWN,
            1,
            self._request(100, b"", True, 100),
            shutdown,
        )
        response = self.wire.decode_ipc_response(execution.response)
        self.assertEqual(events, ["cleanup"])
        self.assertEqual(response.outcome, self.wire.WIRE_OUTCOME_COMMITTED)
        self.assertEqual(response.payload, b"\x01")
        self.assertTrue(shutdown.is_set())
        self.assertFalse(execution.keep_running)

    def test_shutdown_cleanup_exception_returns_unknown_and_stays_retryable(self) -> None:
        shutdown = threading.Event()
        executor = self.rt._HelperRequestExecutor(
            cleanup=lambda: (_ for _ in ()).throw(RuntimeError("cleanup")),
            now_ns=lambda: 1,
        )
        execution = executor.execute(
            self.rt._HELPER_CMD_SHUTDOWN,
            1,
            self._request(101, b"", True, 100),
            shutdown,
        )
        response = self.wire.decode_ipc_response(execution.response)
        self.assertEqual(response.outcome, self.wire.WIRE_OUTCOME_UNKNOWN)
        self.assertEqual(response.payload, b"\x00")
        self.assertFalse(shutdown.is_set())
        self.assertTrue(execution.keep_running)
        self.assertFalse(execution.cleanup_confirmed)

    def test_shutdown_unknown_can_retry_with_a_new_request_id(self) -> None:
        attempts = []
        shutdown = threading.Event()

        def cleanup():
            attempts.append(len(attempts) + 1)
            return len(attempts) >= 2

        executor = self.rt._HelperRequestExecutor(
            cleanup=cleanup,
            now_ns=lambda: 1,
        )
        first = executor.execute(
            self.rt._HELPER_CMD_SHUTDOWN,
            1,
            self._request(102, b"", True, 100),
            shutdown,
        )
        self.assertTrue(first.keep_running)
        self.assertFalse(shutdown.is_set())
        second = executor.execute(
            self.rt._HELPER_CMD_SHUTDOWN,
            2,
            self._request(103, b"", True, 100),
            shutdown,
        )
        response = self.wire.decode_ipc_response(second.response)
        self.assertEqual(attempts, [1, 2])
        self.assertEqual(response.outcome, self.wire.WIRE_OUTCOME_COMMITTED)
        self.assertFalse(second.keep_running)
        self.assertTrue(shutdown.is_set())

    def test_direct_dispatch_cannot_ack_shutdown_before_cleanup(self) -> None:
        sent = []
        event = threading.Event()
        keep_running = self.rt._helper_dispatch(
            self.rt._HELPER_CMD_SHUTDOWN,
            1,
            b"",
            lambda status, seq, payload=b"": sent.append(
                (status, seq, payload)
            ),
            event,
        )
        self.assertTrue(keep_running)
        self.assertEqual(sent, [(1, 1, b"")])
        self.assertFalse(event.is_set())

    def test_init_failure_is_not_overridden_by_residual_handles(self) -> None:
        sent = []
        rt = self.rt
        with (
            mock.patch.object(rt, "ensure_loaded", return_value=False),
            mock.patch.object(rt, "_engine", None),
            mock.patch.object(rt, "_backend", 0),
            mock.patch.object(rt, "_r1h", 123),
            mock.patch.object(rt, "_r3h", None),
            mock.patch.object(rt, "_r5ph", None),
            mock.patch.object(rt, "_r5_ready", False),
            mock.patch.object(rt, "_OFF_PID", 1),
            mock.patch.object(rt, "_resolve_ep_offsets", return_value=False),
        ):
            keep_running = rt._helper_dispatch(
                rt._HELPER_CMD_INIT,
                17,
                struct.pack("<H", 0),
                lambda status, seq, payload=b"": sent.append(
                    (status, seq, payload)
                ),
                threading.Event(),
            )
        self.assertTrue(keep_running)
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0][:2], (0, 17))
        self.assertEqual(sent[0][2][:1], b"\x00")


class PhysicalBatchTransactionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.rt = _backend()
        self.wire = _atomic()

    @staticmethod
    def _reader(memory):
        return lambda address: struct.pack("<I", memory[address])

    def test_transaction_commits_only_after_every_readback(self) -> None:
        memory = {1: 10, 2: 20}

        def write(address, value):
            memory[address] = value
            return True

        outcome, results = self.rt._helper_pw_batch_transaction(
            [(1, 100), (2, 200)],
            read_fn=self._reader(memory),
            write_fn=write,
        )
        self.assertEqual(outcome, self.wire.WIRE_OUTCOME_COMMITTED)
        self.assertEqual(results, b"\x01\x01")
        self.assertEqual(memory, {1: 100, 2: 200})

    def test_failed_item_rolls_back_all_attempted_writes(self) -> None:
        memory = {1: 10, 2: 20}

        def write(address, value):
            if address == 2 and value == 200:
                return False
            memory[address] = value
            return True

        outcome, results = self.rt._helper_pw_batch_transaction(
            [(1, 100), (2, 200)],
            read_fn=self._reader(memory),
            write_fn=write,
        )
        self.assertEqual(outcome, self.wire.WIRE_OUTCOME_ROLLED_BACK)
        self.assertEqual(results, b"\x00\x00")
        self.assertEqual(memory, {1: 10, 2: 20})

    def test_unconfirmed_rollback_is_unknown(self) -> None:
        memory = {1: 10, 2: 20}

        def write(address, value):
            if address == 2 and value == 200:
                memory[address] = value
                return False
            if address == 2 and value == 20:
                return False
            memory[address] = value
            return True

        outcome, _results = self.rt._helper_pw_batch_transaction(
            [(1, 100), (2, 200)],
            read_fn=self._reader(memory),
            write_fn=write,
        )
        self.assertEqual(outcome, self.wire.WIRE_OUTCOME_UNKNOWN)
        self.assertEqual(memory[1], 10)
        self.assertEqual(memory[2], 200)

    def test_expected_value_change_rolls_back_prior_items(self) -> None:
        memory = {1: 10, 2: 20}
        reads_of_two = 0

        def read(address):
            nonlocal reads_of_two
            if address == 2:
                reads_of_two += 1
                if reads_of_two == 2:
                    memory[2] = 21
            return struct.pack("<I", memory[address])

        def write(address, value):
            memory[address] = value
            return True

        outcome, _results = self.rt._helper_pw_batch_transaction(
            [(1, 100), (2, 200)], read_fn=read, write_fn=write
        )
        self.assertEqual(outcome, self.wire.WIRE_OUTCOME_ROLLED_BACK)
        self.assertEqual(memory, {1: 10, 2: 21})

    def test_caller_expected_mismatch_rejects_before_any_write(self) -> None:
        memory = {1: 10, 2: 20}
        writes = []

        def write(address, value):
            writes.append((address, value))
            memory[address] = value
            return True

        outcome, results = self.rt._helper_pw_batch_transaction(
            [(1, 10, 100), (2, 99, 200)],
            read_fn=self._reader(memory),
            write_fn=write,
        )
        self.assertEqual(outcome, self.wire.WIRE_OUTCOME_FAILED)
        self.assertEqual(results, b"\x00\x00")
        self.assertEqual(writes, [])
        self.assertEqual(memory, {1: 10, 2: 20})

    def test_proxy_encodes_strict_expected_current_records(self) -> None:
        proxy = _proxy()
        captured = []

        def committed(command, payload=b"", timeout=None):
            captured.append((command, payload))
            return proxy.IpcCallResult(
                proxy.RequestOutcome.COMMITTED,
                payload=b"\x01\x01",
                authenticated=True,
                transport_complete=True,
                seq_matched=True,
                request_id_matched=True,
            )

        with mock.patch.object(proxy, "_call_result", side_effect=committed):
            result = proxy._pw_batch_tx([
                (1, 10, 100),
                (2, 20, 200),
            ])
        self.assertEqual(result["outcome"], "committed")
        self.assertEqual(result["results"], [True, True])
        self.assertEqual(captured[0][0], proxy.CMD_PW_BATCH_TX)
        self.assertEqual(len(captured[0][1]), 4 + 2 * 16)
        self.assertEqual(
            struct.unpack_from("<QII", captured[0][1], 4),
            (1, 10, 100),
        )

    def test_proxy_exposes_unknown_without_retry(self) -> None:
        proxy = _proxy()
        calls = []

        def unknown(*args, **kwargs):
            calls.append((args, kwargs))
            return proxy.IpcCallResult(proxy.RequestOutcome.UNKNOWN)

        with mock.patch.object(proxy, "_call_result", side_effect=unknown):
            result = proxy._pw_batch_tx([(1, 100), (2, 200)])
        self.assertEqual(result["outcome"], "unknown")
        self.assertEqual(result["results"], [False, False])
        self.assertEqual(len(calls), 1)


class HelperServerContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.rt = _backend()
        self.wire = _atomic()

    def test_pipe_security_rejects_remote_and_non_parent_clients(self) -> None:
        rt = self.rt
        sid = rt._helper_current_user_sid()
        self.assertTrue(sid.startswith("S-1-"))
        sddl = rt._helper_pipe_sddl("S-1-5-21-100")
        self.assertTrue(sddl.startswith("D:P"))
        self.assertIn(";;;SY", sddl)
        self.assertIn(";;;S-1-5-21-100", sddl)
        self.assertEqual(
            rt._HelperPipeBoundary.PIPE_REJECT_REMOTE_CLIENTS,
            0x00000008,
        )
        self.assertTrue(rt._helper_client_allowed(123, 123))
        self.assertFalse(rt._helper_client_allowed(123, 456))
        self.assertFalse(rt._helper_client_allowed(123, 0))
        attributes, descriptor = rt._helper_security_attributes()
        try:
            self.assertTrue(attributes.lpSecurityDescriptor)
        finally:
            rt.kernel32.LocalFree(descriptor)

    def test_disconnected_slot_is_recreated_and_shutdown_ack_follows_cleanup(self) -> None:
        rt = self.rt
        wire = self.wire
        shutdown_envelope = wire.encode_ipc_request(
            77,
            time.monotonic_ns() + 2_000_000_000,
            b"",
            True,
        )
        shutdown_raw = struct.pack(
            "<BI", rt._HELPER_CMD_SHUTDOWN, 7
        ) + shutdown_envelope

        class FakeBoundary:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid
                self.create_count = 0
                self.delivered = False
                self.sent = []
                self.closed_security = False

            def create_pipe(self, pipe_name):
                self.create_count += 1
                return self.create_count

            def connect(self, handle):
                return True

            def client_pid(self, handle):
                return self.parent_pid

            def recv(self, handle, key):
                if handle == 1:
                    return None
                if not self.delivered:
                    self.delivered = True
                    return shutdown_raw
                return None

            def send(self, handle, key, status, seq, payload):
                self.sent.append((status, seq, payload))
                return True

            def cancel(self, handle):
                return None

            def disconnect_close(self, handle):
                return None

            def parent_alive(self):
                return True

            def close_security(self):
                self.closed_security = True

        boundary = FakeBoundary(321)
        cleanup_events = []

        def executor_factory():
            return rt._HelperRequestExecutor(
                cleanup=lambda: cleanup_events.append("cleanup") or True
            )

        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            worker = threading.Thread(
                target=lambda: rt._helper_main(
                    "test-pipe",
                    (b"k" * 32).hex(),
                    parent_pid=321,
                    boundary_factory=lambda _pid: boundary,
                    executor_factory=executor_factory,
                )
            )
            worker.start()
            worker.join(3.0)

        self.assertFalse(worker.is_alive())
        self.assertGreaterEqual(boundary.create_count, 2)
        self.assertEqual(cleanup_events, ["cleanup"])
        self.assertTrue(boundary.closed_security)
        self.assertEqual(len(boundary.sent), 1)
        status, seq, payload = boundary.sent[0]
        self.assertEqual((status, seq), (0, 7))
        decoded = wire.decode_ipc_response(payload)
        self.assertEqual(decoded.outcome, wire.WIRE_OUTCOME_COMMITTED)
        self.assertEqual(decoded.payload, b"\x01")

    def test_parent_death_enters_the_same_serial_cleanup_path(self) -> None:
        rt = self.rt

        class DeadParentBoundary:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid
                self.cancelled = threading.Event()
                self.closed_security = False

            def create_pipe(self, pipe_name):
                return 1

            def connect(self, handle):
                return True

            def client_pid(self, handle):
                return self.parent_pid

            def recv(self, handle, key):
                self.cancelled.wait(1.0)
                return None

            def send(self, *args):
                return True

            def cancel(self, handle):
                self.cancelled.set()

            def disconnect_close(self, handle):
                self.cancelled.set()

            def parent_alive(self):
                return False

            def close_security(self):
                self.closed_security = True

        boundary = DeadParentBoundary(500)
        cleanup = []
        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            thread = threading.Thread(target=lambda: rt._helper_main(
                "test-pipe",
                (b"p" * 32).hex(),
                parent_pid=500,
                boundary_factory=lambda _pid: boundary,
                executor_factory=lambda: rt._HelperRequestExecutor(
                    cleanup=lambda: cleanup.append("cleanup") or True
                ),
                monitor_interval=0.01,
            ))
            thread.start()
            thread.join(2.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(cleanup, ["cleanup"])
        self.assertTrue(boundary.closed_security)

    def test_parent_death_bounds_unknown_cleanup_retries_and_does_not_orphan(self) -> None:
        rt = self.rt

        class DeadParentBoundary:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid
                self.cancelled = threading.Event()

            def create_pipe(self, pipe_name):
                return 1

            def connect(self, handle):
                return True

            def client_pid(self, handle):
                return self.parent_pid

            def recv(self, handle, key):
                self.cancelled.wait(1.0)
                return None

            def send(self, *args):
                return True

            def cancel(self, handle):
                self.cancelled.set()

            def disconnect_close(self, handle):
                self.cancelled.set()

            def parent_alive(self):
                return False

            def close_security(self):
                return None

        boundary = DeadParentBoundary(501)
        cleanup = []
        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            thread = threading.Thread(target=lambda: rt._helper_main(
                "test-pipe",
                (b"u" * 32).hex(),
                parent_pid=501,
                boundary_factory=lambda _pid: boundary,
                executor_factory=lambda: rt._HelperRequestExecutor(
                    cleanup=lambda: cleanup.append("cleanup") or False
                ),
                monitor_interval=0.01,
                orphan_cleanup_attempts=3,
                orphan_cleanup_backoff=0.001,
            ))
            thread.start()
            thread.join(2.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(cleanup, ["cleanup", "cleanup", "cleanup"])

    def test_parent_death_does_not_wait_forever_for_stuck_cleanup(self) -> None:
        rt = self.rt
        cleanup_entered = threading.Event()
        release_cleanup = threading.Event()

        class DeadParentWithoutClients:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid

            def create_pipe(self, pipe_name):
                return None

            def cancel(self, handle):
                return None

            def parent_alive(self):
                return False

            def close_security(self):
                return None

        def cleanup():
            cleanup_entered.set()
            release_cleanup.wait(2.0)
            return True

        boundary = DeadParentWithoutClients(502)
        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            thread = threading.Thread(target=lambda: rt._helper_main(
                "test-pipe",
                (b"h" * 32).hex(),
                parent_pid=502,
                boundary_factory=lambda _pid: boundary,
                executor_factory=lambda: rt._HelperRequestExecutor(
                    cleanup=cleanup
                ),
                monitor_interval=0.01,
                orphan_cleanup_attempts=1,
                orphan_cleanup_backoff=0.001,
            ))
            thread.start()
            try:
                self.assertTrue(cleanup_entered.wait(1.0))
                thread.join(0.25)
                self.assertFalse(
                    thread.is_alive(),
                    "parent-death teardown must bound a stuck executor cleanup",
                )
            finally:
                release_cleanup.set()
                thread.join(2.0)

    def test_executor_fatal_error_runs_emergency_cleanup_and_exits(self) -> None:
        rt = self.rt
        wire = self.wire
        ping = struct.pack("<BI", rt._HELPER_CMD_PING, 1) + (
            wire.encode_ipc_request(
                71,
                time.monotonic_ns() + 1_000_000_000,
                b"",
                False,
            )
        )

        class Boundary:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid
                self.delivered = False
                self.cancelled = threading.Event()

            def create_pipe(self, pipe_name):
                return 1

            def connect(self, handle):
                return True

            def client_pid(self, handle):
                return self.parent_pid

            def recv(self, handle, key):
                if not self.delivered:
                    self.delivered = True
                    return ping
                self.cancelled.wait(1.0)
                return None

            def send(self, *args):
                raise AssertionError("fatal executor must not send an ACK")

            def cancel(self, handle):
                self.cancelled.set()

            def disconnect_close(self, handle):
                self.cancelled.set()

            def parent_alive(self):
                return True

            def close_security(self):
                return None

        cleanup = []

        class ExplodingExecutor:
            def execute(self, *args):
                raise RuntimeError("executor failure")

            def cancel(self, envelope):
                raise RuntimeError("executor failure")

            def emergency_cleanup(self):
                cleanup.append("cleanup")
                return True

        boundary = Boundary(800)
        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            thread = threading.Thread(target=lambda: rt._helper_main(
                "test-pipe",
                (b"f" * 32).hex(),
                parent_pid=800,
                boundary_factory=lambda _pid: boundary,
                executor_factory=ExplodingExecutor,
            ))
            thread.start()
            thread.join(2.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(cleanup, ["cleanup"])

    def test_all_disconnected_clients_trigger_orphan_cleanup(self) -> None:
        rt = self.rt

        class OrphanBoundary:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid
                self.connect_count = 0
                self.cancelled = threading.Event()

            def create_pipe(self, pipe_name):
                return self.connect_count + 1

            def connect(self, handle):
                self.connect_count += 1
                if self.connect_count == 1:
                    return True
                time.sleep(0.002)
                return False

            def client_pid(self, handle):
                return self.parent_pid

            def recv(self, handle, key):
                return None

            def send(self, *args):
                return True

            def cancel(self, handle):
                self.cancelled.set()

            def disconnect_close(self, handle):
                return None

            def parent_alive(self):
                return True

            def close_security(self):
                return None

        boundary = OrphanBoundary(600)
        cleanup = []
        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            thread = threading.Thread(target=lambda: rt._helper_main(
                "test-pipe",
                (b"o" * 32).hex(),
                parent_pid=600,
                boundary_factory=lambda _pid: boundary,
                executor_factory=lambda: rt._HelperRequestExecutor(
                    cleanup=lambda: cleanup.append("cleanup") or True
                ),
                orphan_grace_seconds=0.03,
                monitor_interval=0.01,
            ))
            thread.start()
            thread.join(2.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(cleanup, ["cleanup"])
        self.assertGreater(boundary.connect_count, 1)

    def test_never_connected_startup_self_cleans_instead_of_orphaning(self) -> None:
        rt = self.rt

        class NeverConnectedBoundary:
            def __init__(self, parent_pid):
                self.parent_pid = parent_pid
                self.cancelled = threading.Event()

            def create_pipe(self, pipe_name):
                return 1

            def connect(self, handle):
                time.sleep(0.002)
                return False

            def client_pid(self, handle):
                return 0

            def recv(self, handle, key):
                raise AssertionError("an unconnected pipe must not be read")

            def send(self, *args):
                return True

            def cancel(self, handle):
                self.cancelled.set()

            def disconnect_close(self, handle):
                return None

            def parent_alive(self):
                return True

            def close_security(self):
                return None

        boundary = NeverConnectedBoundary(700)
        cleanup = []
        with mock.patch.object(rt, "_HELPER_PIPE_INSTANCES", 1):
            thread = threading.Thread(target=lambda: rt._helper_main(
                "test-pipe",
                (b"s" * 32).hex(),
                parent_pid=700,
                boundary_factory=lambda _pid: boundary,
                executor_factory=lambda: rt._HelperRequestExecutor(
                    cleanup=lambda: cleanup.append("cleanup") or True
                ),
                monitor_interval=0.01,
                startup_grace_seconds=0.03,
            ))
            thread.start()
            thread.join(2.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(cleanup, ["cleanup"])
        self.assertTrue(boundary.cancelled.is_set())


if __name__ == "__main__":
    unittest.main(verbosity=2)
