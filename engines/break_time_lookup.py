# -*- coding: utf-8 -*-
"""break_time_lookup - MonsterTable BreakingContinueTime lookup by template_id.

Loads the break recovery time from the offline raid mechanics dump. When a boss
enters break state, this value gives the total duration of the break window
(bar animates 0%→100% over this time).

Resolution order:
  1. Offline dump (exports/boss_raids/full_raid_mechanics_dump.json)
  2. Returns 0.0 if unknown (caller falls back to old multi-phase behavior)
"""
from __future__ import annotations

import json
import logging
import os
from typing import Dict

logger = logging.getLogger(__name__)

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
_DUMP_PATH = os.path.join(_ROOT, "exports", "boss_raids", "full_raid_mechanics_dump.json")

_cache: Dict[int, float] = {}
_loaded = False


def _load_dump() -> None:
    global _loaded
    if _loaded:
        return
    _loaded = True
    if not os.path.isfile(_DUMP_PATH):
        return
    try:
        with open(_DUMP_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        _walk(data.get("raids", {}))
        if _cache:
            logger.debug(f"[BreakTimeLookup] Loaded {len(_cache)} boss entries")
    except Exception as e:
        logger.debug(f"[BreakTimeLookup] Failed to load dump: {e}")


def _walk(obj) -> None:
    if isinstance(obj, dict):
        if "BreakingContinueTime" in obj and "id" in obj:
            try:
                tid = int(obj["id"])
                bct = float(obj["BreakingContinueTime"])
                if tid > 0 and bct > 0:
                    _cache[tid] = bct
            except (ValueError, TypeError):
                pass
        for v in obj.values():
            _walk(v)
    elif isinstance(obj, list):
        for v in obj:
            _walk(v)


def get_break_recovery_time(template_id: int) -> float:
    """Return BreakingContinueTime in seconds for a boss template_id, or 0.0 if unknown."""
    if not template_id:
        return 0.0
    _load_dump()
    return _cache.get(int(template_id), 0.0)
