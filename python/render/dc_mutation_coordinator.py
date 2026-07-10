"""Serialized, generation-aware display-context mutations for overlay HWNDs.

The compositor and Tk threads submit work here instead of blocking on the
rt_io pipe.  Calls with the same ``(hwnd, generation, operation)`` key are
coalesced while an earlier call is in flight.  No helper/backend import occurs
until work is actually submitted.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import threading
import time
from typing import Any, Callable, Optional


@dataclass(frozen=True)
class _Mutation:
    token: Any
    key: tuple[int, int, str]
    fn: Callable[..., Any]
    args: tuple
    kwargs: dict


class InvalidationBarrier:
    """Completion token for a revoked HWND generation."""

    def __init__(self) -> None:
        self._event = threading.Event()
        self.confirmed = False

    def wait(self, timeout: Optional[float] = None) -> bool:
        return bool(self._event.wait(timeout) and self.confirmed)

    @property
    def done(self) -> bool:
        return self._event.is_set()

    def _finish(self, confirmed: bool) -> None:
        self.confirmed = bool(confirmed)
        self._event.set()


class DcMutationCoordinator:
    """Own one mutation worker and make HWND teardown an explicit barrier."""

    def __init__(self, dc_module=None) -> None:
        self._dc_module = dc_module
        self._condition = threading.Condition(threading.RLock())
        self._pending: dict[tuple[int, int, str], _Mutation] = {}
        self._queue: deque[tuple[int, int, str]] = deque()
        self._queued: set[tuple[int, int, str]] = set()
        self._inflight: dict[int, int] = {}
        self._tokens: dict[int, Any] = {}
        self._invalidations = 0
        self._invalidation_failed = False
        self._failed_invalidations: set[int] = set()
        self._accepting = True
        self._stop_requested = False
        self._thread = threading.Thread(
            target=self._run, name="DcMutationCoordinator", daemon=True)
        self._thread.start()

    def _dc(self):
        if self._dc_module is None:
            from mem_probe import _dc
            self._dc_module = _dc
        return self._dc_module

    def register(self, hwnd: int):
        hwnd = int(hwnd or 0)
        if not hwnd:
            return None
        token = self._dc().register_window(hwnd)
        if token is not None:
            with self._condition:
                self._tokens[hwnd] = token
        return token

    def submit(self, hwnd: int, operation: str,
               fn: Callable[..., Any], *args, **kwargs) -> bool:
        hwnd = int(hwnd or 0)
        if not hwnd:
            return False
        with self._condition:
            if not self._accepting:
                return False
            token = self._tokens.get(hwnd)
        if token is None:
            token = self.register(hwnd)
        if token is None:
            return False
        generation = int(getattr(token, "generation", 0))
        key = (hwnd, generation, str(operation))
        task = _Mutation(token, key, fn, tuple(args), dict(kwargs))
        with self._condition:
            if not self._accepting:
                return False
            self._pending[key] = task
            if key not in self._queued:
                self._queue.append(key)
                self._queued.add(key)
            self._condition.notify_all()
        return True

    def submit_dc(self, hwnd: int, operation: str,
                  method_name: str, *args, **kwargs) -> bool:
        with self._condition:
            if not self._accepting:
                return False
        try:
            dc = self._dc()
        except Exception:
            return False
        fn = getattr(dc, method_name, None)
        if not callable(fn):
            return False
        return self.submit(hwnd, operation, fn, int(hwnd), *args, **kwargs)

    def drain(self, hwnd: Optional[int] = None,
              timeout: Optional[float] = None) -> bool:
        deadline = None if timeout is None else time.monotonic() + max(0, timeout)
        target = None if hwnd is None else int(hwnd)
        with self._condition:
            while True:
                if target is None:
                    pending = bool(self._pending)
                    inflight = any(self._inflight.values())
                else:
                    pending = any(key[0] == target for key in self._pending)
                    inflight = self._inflight.get(target, 0) > 0
                if not pending and not inflight:
                    return True
                if deadline is None:
                    self._condition.wait()
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._condition.wait(remaining)

    def invalidate(self, hwnd: int, timeout: Optional[float] = None) -> bool:
        barrier = self.begin_invalidate(hwnd, timeout=timeout)
        return barrier.wait(timeout)

    def begin_invalidate(self, hwnd: int,
                         timeout: Optional[float] = None
                         ) -> InvalidationBarrier:
        """Revoke now and drain on a helper thread.

        The caller can withdraw an input proxy immediately and poll the
        returned barrier from Tk without ever waiting on a helper pipe.
        """
        hwnd = int(hwnd or 0)
        barrier = InvalidationBarrier()
        if not hwnd:
            barrier._finish(True)
            return barrier
        with self._condition:
            token = self._tokens.pop(hwnd, None)
            for key in [key for key in self._pending if key[0] == hwnd]:
                self._pending.pop(key, None)
            self._invalidations += 1
            self._condition.notify_all()

        try:
            dc = self._dc()
        except Exception:
            with self._condition:
                self._invalidations = max(0, self._invalidations - 1)
                self._failed_invalidations.add(hwnd)
                self._invalidation_failed = True
                self._condition.notify_all()
            barrier._finish(False)
            return barrier
        revoke = getattr(dc, 'revoke_window', None)
        revoked = token is None
        deferred_invalidate = token is not None and not callable(revoke)
        if token is not None:
            try:
                if callable(revoke):
                    revoked = bool(revoke(token))
            except Exception:
                revoked = False

        def _drain() -> None:
            deadline = (None if timeout is None else
                        time.monotonic() + max(0.0, timeout))
            confirmed = False
            try:
                remaining = (None if deadline is None else
                             max(0.0, deadline - time.monotonic()))
                drained = self.drain(hwnd, timeout=remaining)
                underlying = True
                if token is not None and drained:
                    if deferred_invalidate:
                        underlying = bool(dc.invalidate_window(token))
                    else:
                        fn = getattr(dc, 'drain_window_mutations', None)
                        underlying = bool(not callable(fn) or fn(token))
                confirmed = bool(revoked and drained and underlying)
            except Exception:
                confirmed = False
            finally:
                with self._condition:
                    self._invalidations = max(0, self._invalidations - 1)
                    if confirmed:
                        self._failed_invalidations.discard(hwnd)
                    else:
                        self._failed_invalidations.add(hwnd)
                    self._invalidation_failed = bool(self._failed_invalidations)
                    self._condition.notify_all()
                barrier._finish(confirmed)

        try:
            threading.Thread(
                target=_drain, name=f'DcMutationDrain-{hwnd:x}',
                daemon=True).start()
        except Exception:
            with self._condition:
                self._invalidations = max(0, self._invalidations - 1)
                self._failed_invalidations.add(hwnd)
                self._invalidation_failed = True
                self._condition.notify_all()
            barrier._finish(False)
        return barrier

    def quiesce(self) -> None:
        with self._condition:
            self._accepting = False
            self._condition.notify_all()

    def stop(self, timeout: Optional[float] = 5.0) -> bool:
        self.quiesce()
        deadline = (None if timeout is None else
                    time.monotonic() + max(0.0, timeout))
        with self._condition:
            self._stop_requested = True
            self._condition.notify_all()
        remaining = (None if deadline is None else
                     max(0.0, deadline - time.monotonic()))
        self._thread.join(timeout=remaining)
        if self._thread.is_alive():
            return False
        with self._condition:
            while self._invalidations:
                if deadline is None:
                    self._condition.wait()
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._condition.wait(remaining)
        return not self._failed_invalidations

    def is_alive(self) -> bool:
        return self._thread.is_alive()

    def _run(self) -> None:
        while True:
            with self._condition:
                while not self._queue:
                    if self._stop_requested and not self._pending:
                        return
                    self._condition.wait()
                key = self._queue.popleft()
                self._queued.discard(key)
                task = self._pending.pop(key, None)
                if task is None:
                    self._condition.notify_all()
                    continue
                hwnd = key[0]
                self._inflight[hwnd] = self._inflight.get(hwnd, 0) + 1
            try:
                if self._dc()._validate_window_token(task.token):
                    task.fn(*task.args, **task.kwargs)
            except Exception:
                pass
            finally:
                with self._condition:
                    count = self._inflight.get(hwnd, 1) - 1
                    if count > 0:
                        self._inflight[hwnd] = count
                    else:
                        self._inflight.pop(hwnd, None)
                    self._condition.notify_all()
