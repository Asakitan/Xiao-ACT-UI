# -*- coding: utf-8 -*-
"""Small ACT event bus with callback isolation.

The bus is synchronous by default so replay tests are deterministic.  Callback
errors are isolated and recorded instead of bubbling into parser/DPS code.
"""

from __future__ import annotations

import copy
import threading
import time
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Mapping, Optional

from .events import clone_event, is_event_envelope, make_event


EventCallback = Callable[[dict[str, Any]], None]


@dataclass
class Subscription:
    token: str
    topic: str
    callback: EventCallback
    owner_id: str = ""
    active: bool = True
    deliveries: int = 0
    failures: int = 0
    total_callback_ms: float = 0.0
    last_error: str = ""


class EventBus:
    """Thread-safe publish/subscribe bus for canonical ACT events."""

    def __init__(self, max_recent: int = 200, slow_callback_ms: float = 25.0,
                 ephemeral_topics: Optional[set[str]] = None) -> None:
        self._lock = threading.RLock()
        self._subscriptions: Dict[str, List[Subscription]] = {}
        self._by_token: Dict[str, Subscription] = {}
        self._recent: List[dict[str, Any]] = []
        self._max_recent = max(1, int(max_recent or 200))
        self._slow_callback_ms = max(0.0, float(slow_callback_ms or 0.0))
        # Ephemeral topics are delivered to subscribers but NOT retained in the
        # `_recent` ring. ``act_snapshot`` is the canonical case: it is a large
        # nested payload published at combat rate purely for live overlay/plugin
        # consumption and is already filtered out of the action-log / aggregate
        # views, so deep-cloning it into the ring every push was pure waste
        # (one extra copy.deepcopy per push + churning the whole ring).
        self._ephemeral_topics: set[str] = set(ephemeral_topics or ())
        self._published = 0
        self._retained = 0  # increments only for events actually kept in _recent
        self._callback_failures = 0
        self._slow_callbacks = 0

    def subscribe(self, topic: str, callback: EventCallback,
                  owner_id: str = "") -> str:
        if not callable(callback):
            raise TypeError("ACT event callback must be callable")
        topic = str(topic or "*")
        sub = Subscription(
            token=uuid.uuid4().hex,
            topic=topic,
            callback=callback,
            owner_id=str(owner_id or ""),
        )
        with self._lock:
            self._subscriptions.setdefault(topic, []).append(sub)
            self._by_token[sub.token] = sub
        return sub.token

    @property
    def published(self) -> int:
        """Monotonic count of all published events."""
        return self._published

    @property
    def retained(self) -> int:
        """Monotonic count of events actually kept in ``_recent`` (excludes
        ephemeral topics like act_snapshot). This is the precise freshness token
        for the aggregate, which folds only the retained slice — so an unchanged
        value means the aggregate cannot have changed, even if high-rate
        ephemeral pushes bumped ``published``."""
        return self._retained

    def unsubscribe(self, token: str) -> bool:
        token = str(token or "")
        with self._lock:
            sub = self._by_token.pop(token, None)
            if sub is None:
                return False
            sub.active = False
            bucket = self._subscriptions.get(sub.topic) or []
            self._subscriptions[sub.topic] = [item for item in bucket if item.token != token]
            return True

    def publish(self, topic: str, payload: Optional[Mapping[str, Any]] = None,
                *, event: Optional[Mapping[str, Any]] = None,
                source_name: str = "unknown", source_kind: str = "unknown",
                game_id: str = "star_resonance", parser_id: str = "",
                confidence: float = 1.0) -> dict[str, Any]:
        if event is not None and is_event_envelope(event):
            envelope = clone_event(event)
            topic = str(envelope.get("topic") or topic or "parsed_event")
        else:
            envelope = make_event(
                topic,
                payload,
                source_name=source_name,
                source_kind=source_kind,
                game_id=game_id,
                parser_id=parser_id,
                confidence=confidence,
            )

        with self._lock:
            self._published += 1
            if topic not in self._ephemeral_topics:
                self._retained += 1
                self._recent.append(clone_event(envelope))
                if len(self._recent) > self._max_recent:
                    self._recent = self._recent[-self._max_recent:]
            callbacks = list(self._subscriptions.get(topic, ())) + list(self._subscriptions.get("*", ()))

        for sub in callbacks:
            if not sub.active:
                continue
            start = time.perf_counter()
            try:
                sub.callback(clone_event(envelope))
                elapsed_ms = (time.perf_counter() - start) * 1000.0
                with self._lock:
                    sub.deliveries += 1
                    sub.total_callback_ms += elapsed_ms
                    if self._slow_callback_ms and elapsed_ms > self._slow_callback_ms:
                        self._slow_callbacks += 1
            except Exception as exc:
                elapsed_ms = (time.perf_counter() - start) * 1000.0
                with self._lock:
                    sub.failures += 1
                    sub.total_callback_ms += elapsed_ms
                    sub.last_error = str(exc)
                    self._callback_failures += 1
        return envelope

    def recent_events(self, limit: int = 20) -> list[dict[str, Any]]:
        cap = max(0, int(limit or 20))
        with self._lock:
            return [clone_event(item) for item in self._recent[-cap:]][::-1]

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            topics = {
                topic: len([sub for sub in subs if sub.active])
                for topic, subs in self._subscriptions.items()
            }
            subscriptions = [
                {
                    "token": sub.token,
                    "topic": sub.topic,
                    "owner_id": sub.owner_id,
                    "active": sub.active,
                    "deliveries": sub.deliveries,
                    "failures": sub.failures,
                    "last_error": sub.last_error,
                    "total_callback_ms": round(sub.total_callback_ms, 3),
                }
                for sub in self._by_token.values()
            ]
            return {
                "published": self._published,
                "callback_failures": self._callback_failures,
                "slow_callbacks": self._slow_callbacks,
                "topics": topics,
                "subscriptions": copy.deepcopy(subscriptions),
                "recent_count": len(self._recent),
            }


__all__ = ["EventBus", "Subscription"]