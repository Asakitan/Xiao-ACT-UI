"""No-driver behavioral tests for the overlay dc mutation coordinator."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from render.dc_mutation_coordinator import DcMutationCoordinator


@dataclass(frozen=True)
class Token:
    hwnd: int
    generation: int


class FakeDc:
    def __init__(self) -> None:
        self.tokens = {}
        self.invalidated = []

    def register_window(self, hwnd):
        return self.tokens.setdefault(int(hwnd), Token(int(hwnd), 1))

    def _validate_window_token(self, token):
        return self.tokens.get(token.hwnd) == token

    def invalidate_window(self, token):
        self.invalidated.append(token)
        self.tokens.pop(token.hwnd, None)
        return True

    def revoke_window(self, token):
        self.invalidated.append(token)
        return self.tokens.pop(token.hwnd, None) == token

    def drain_window_mutations(self, _token):
        return True


class DcMutationCoordinatorTests(unittest.TestCase):
    def test_pending_mutations_with_same_key_are_coalesced(self) -> None:
        dc = FakeDc()
        coordinator = DcMutationCoordinator(dc_module=dc)
        started = threading.Event()
        release = threading.Event()
        calls = []

        def operation(value):
            calls.append(value)
            if value == 1:
                started.set()
                release.wait(2)
            return True

        self.assertTrue(coordinator.submit(10, "exstyle", operation, 1))
        self.assertTrue(started.wait(1))
        self.assertTrue(coordinator.submit(10, "exstyle", operation, 2))
        self.assertTrue(coordinator.submit(10, "exstyle", operation, 3))
        release.set()
        self.assertTrue(coordinator.drain(10, timeout=2))
        self.assertEqual(calls, [1, 3])
        self.assertTrue(coordinator.stop(timeout=2))

    def test_invalidate_waits_for_inflight_mutation(self) -> None:
        dc = FakeDc()
        coordinator = DcMutationCoordinator(dc_module=dc)
        started = threading.Event()
        release = threading.Event()

        def operation():
            started.set()
            release.wait(2)
            return True

        self.assertTrue(coordinator.submit(20, "rect", operation))
        self.assertTrue(started.wait(1))
        done = threading.Event()

        def invalidate():
            coordinator.invalidate(20, timeout=2)
            done.set()

        thread = threading.Thread(target=invalidate)
        thread.start()
        time.sleep(0.03)
        self.assertFalse(done.is_set())
        release.set()
        thread.join(timeout=2)
        self.assertTrue(done.is_set())
        self.assertEqual(len(dc.invalidated), 1)
        self.assertTrue(coordinator.stop(timeout=2))

    def test_begin_invalidate_revokes_immediately_but_drains_async(self) -> None:
        dc = FakeDc()
        coordinator = DcMutationCoordinator(dc_module=dc)
        started = threading.Event()
        release = threading.Event()

        def operation():
            started.set()
            release.wait(2)
            return True

        self.assertTrue(coordinator.submit(25, "rect", operation))
        self.assertTrue(started.wait(1))
        before = time.monotonic()
        barrier = coordinator.begin_invalidate(25, timeout=2)
        self.assertLess(time.monotonic() - before, 0.1)
        self.assertNotIn(25, dc.tokens)
        self.assertFalse(barrier.wait(0.02))
        release.set()
        self.assertTrue(barrier.wait(2))
        self.assertTrue(barrier.confirmed)
        self.assertTrue(coordinator.stop(timeout=2))

    def test_begin_invalidate_cannot_be_bypassed_by_late_registration(self) -> None:
        """A teardown barrier must close the first-register race for its HWND."""
        register_started = threading.Event()
        allow_register = threading.Event()
        calls = []

        class BlockingRegisterDc(FakeDc):
            def register_window(self, hwnd):
                register_started.set()
                allow_register.wait(2)
                return super().register_window(hwnd)

        dc = BlockingRegisterDc()
        coordinator = DcMutationCoordinator(dc_module=dc)
        result = []

        def submit_late() -> None:
            result.append(coordinator.submit(
                27, "rect", lambda: calls.append("mutated") or True))

        submitter = threading.Thread(target=submit_late)
        submitter.start()
        try:
            self.assertTrue(register_started.wait(1))
            barrier = coordinator.begin_invalidate(27, timeout=1)
            self.assertTrue(barrier.wait(1))
            allow_register.set()
            submitter.join(timeout=1)

            self.assertFalse(submitter.is_alive())
            self.assertEqual(result, [False])
            self.assertTrue(coordinator.drain(27, timeout=1))
            self.assertEqual(calls, [])
            self.assertNotIn(27, dc.tokens)
        finally:
            allow_register.set()
            submitter.join(timeout=1)
            coordinator.quiesce()
            coordinator.stop(timeout=2)

    def test_quiesce_rejects_new_work_and_stop_is_proven(self) -> None:
        dc = FakeDc()
        coordinator = DcMutationCoordinator(dc_module=dc)
        coordinator.quiesce()
        self.assertFalse(coordinator.submit(30, "exstyle", lambda: True))
        self.assertTrue(coordinator.stop(timeout=2))
        self.assertFalse(coordinator.is_alive())


if __name__ == "__main__":
    unittest.main(verbosity=2)
