# -*- coding: utf-8 -*-
"""Normalized ACT replay event helpers.

The parser already emits dictionaries for dungeon and skill lifecycle events.
These helpers keep replay fixtures close to that shape while making small smoke
tests easier to read and harder to typo.
"""

from __future__ import annotations

import time
from typing import Any, Dict


def normalized_event(kind: str, **payload: Any) -> Dict[str, Any]:
    event = {
        "kind": str(kind or ""),
        "timestamp": float(payload.pop("timestamp", time.time())),
        "source": str(payload.pop("source", "replay")),
    }
    event.update(payload)
    return event


def dungeon_event(kind: str, **payload: Any) -> Dict[str, Any]:
    return normalized_event(kind, **payload)


def skill_event(kind: str, **payload: Any) -> Dict[str, Any]:
    return normalized_event(kind, **payload)


def boss_state_event(**payload: Any) -> Dict[str, Any]:
    return normalized_event("boss_state", **payload)


def monster_update_event(**payload: Any) -> Dict[str, Any]:
    return normalized_event("monster_update", **payload)


def damage_event(**payload: Any) -> Dict[str, Any]:
    return normalized_event("damage", **payload)


__all__ = [
    "normalized_event",
    "dungeon_event",
    "skill_event",
    "boss_state_event",
    "monster_update_event",
    "damage_event",
]