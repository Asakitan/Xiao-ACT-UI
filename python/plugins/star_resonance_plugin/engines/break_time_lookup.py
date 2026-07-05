# -*- coding: utf-8 -*-
# break_time_lookup - MonsterTable BreakingContinueTime lookup by template_id.
#
# Resolution order:
# 1. Local cache file (assets/break_time_cache.json) — fast, persistent
# 2. Offline raid dump (exports/boss_raids/full_raid_mechanics_dump.json) — legacy seed
# 3. Live MEM reading via MemConfigTableReader — on cache miss, auto-saves result
#
# The cache covers ALL monsters (not just raid bosses). First run with the game
# attached builds the full cache; subsequent launches use the file directly.
from __future__ import annotations

import json
import logging
import os
import struct
import threading
from typing import Dict, Optional

logger = logging.getLogger(__name__)

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
_CACHE_PATH = os.path.join(_ROOT, "assets", "break_time_cache.json")
_DUMP_PATH = os.path.join(_ROOT, "exports", "boss_raids", "full_raid_mechanics_dump.json")

_cache: Dict[int, float] = {}
_loaded = False
_dirty = False
_lock = threading.Lock()

# MonsterTableBase column offset for BreakingContinueTime (f32)
# Resolved from table_columns cache; 0x148 is the verified fallback.
_COL_BREAKING_CONTINUE_TIME: Optional[int] = None
_MONSTER_CLS = "Bokura.MonsterTableBase"


def _load() -> None:
    global _loaded
    if _loaded:
        return
    _loaded = True
    # 1. Load persistent cache
    if os.path.isfile(_CACHE_PATH):
        try:
            with open(_CACHE_PATH, "r", encoding="utf-8") as f:
                raw = json.load(f)
            for k, v in raw.items():
                tid = int(k)
                val = float(v)
                if tid > 0 and val > 0:
                    _cache[tid] = val
            if _cache:
                logger.debug(f"[BreakTime] Cache loaded: {len(_cache)} entries")
                return
        except Exception as e:
            logger.debug(f"[BreakTime] Cache load failed: {e}")
    # 2. Fallback: seed from raid dump
    if os.path.isfile(_DUMP_PATH):
        try:
            with open(_DUMP_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
            _walk_dump(data.get("raids", {}))
            if _cache:
                logger.debug(f"[BreakTime] Seeded from raid dump: {len(_cache)} entries")
        except Exception:
            pass


def _walk_dump(obj) -> None:
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
            _walk_dump(v)
    elif isinstance(obj, list):
        for v in obj:
            _walk_dump(v)


def _save_cache() -> None:
    global _dirty
    if not _dirty or not _cache:
        return
    try:
        os.makedirs(os.path.dirname(_CACHE_PATH), exist_ok=True)
        with open(_CACHE_PATH, "w", encoding="utf-8") as f:
            json.dump({str(k): v for k, v in sorted(_cache.items())}, f, indent=1)
        _dirty = False
        logger.debug(f"[BreakTime] Cache saved: {len(_cache)} entries")
    except Exception as e:
        logger.debug(f"[BreakTime] Cache save failed: {e}")


def _resolve_col_offset() -> Optional[int]:
    # Resolve the BreakingContinueTime column offset from table_columns.
    global _COL_BREAKING_CONTINUE_TIME
    if _COL_BREAKING_CONTINUE_TIME is not None:
        return _COL_BREAKING_CONTINUE_TIME
    try:
        from plugins.star_resonance_plugin.mem.il2cpp import table_columns
        cols = table_columns.load_columns([_MONSTER_CLS])
        ent = cols.get(_MONSTER_CLS, {}).get("BreakingContinueTime")
        if ent:
            _COL_BREAKING_CONTINUE_TIME = ent[0]
            return _COL_BREAKING_CONTINUE_TIME
    except Exception:
        pass
    _COL_BREAKING_CONTINUE_TIME = 0x148
    return 0x148


def _read_live(template_id: int) -> float:
    # Try to read BreakingContinueTime from live memory for one monster.
    global _dirty
    try:
        from plugins.star_resonance_plugin.mem.il2cpp.mem_config_table_reader import MemConfigTableReader, TABLE_CLASS
        from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
        src = StaticDpsSource()
        _ = src.sr
        rd = MemConfigTableReader(src)
        col = _resolve_col_offset()
        if col is None:
            return 0.0
        for _key, zl, blob in rd.iter_rows_via_loader(_MONSTER_CLS):
            rid = rd.col_i32(blob, 0)
            if rid == template_id:
                val = rd.col_f32(blob, col)
                if val is not None and val > 0:
                    _cache[template_id] = val
                    _dirty = True
                    _save_cache()
                    return val
                return 0.0
    except Exception as e:
        logger.debug(f"[BreakTime] Live read failed for {template_id}: {e}")
    return 0.0


def get_break_recovery_time(template_id: int) -> float:
    # Return BreakingContinueTime in seconds for a monster template_id, or 0.0.
    if not template_id:
        return 0.0
    with _lock:
        _load()
        val = _cache.get(int(template_id))
    if val is not None:
        return val
    # Cache miss → try live MEM (expensive, but result is cached)
    return _read_live(int(template_id))


def build_full_cache() -> int:
    # Iterate ALL MonsterTable rows and cache BreakingContinueTime. Requires game attached.
    #
    # Returns the number of entries written. Call from tools or on first attach.
    global _dirty
    _load()
    try:
        from plugins.star_resonance_plugin.mem.il2cpp.mem_config_table_reader import MemConfigTableReader
        from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
        src = StaticDpsSource()
        _ = src.sr
        rd = MemConfigTableReader(src)
        col = _resolve_col_offset()
        if col is None:
            return 0
        count = 0
        for _key, zl, blob in rd.iter_rows_via_loader(_MONSTER_CLS):
            rid = rd.col_i32(blob, 0)
            if not rid or rid <= 0:
                continue
            val = rd.col_f32(blob, col)
            if val is not None and val > 0:
                if _cache.get(rid) != val:
                    _cache[rid] = val
                    _dirty = True
                    count += 1
        if _dirty:
            _save_cache()
        logger.info(f"[BreakTime] Full cache built: {count} new entries, {len(_cache)} total")
        return count
    except Exception as e:
        logger.error(f"[BreakTime] build_full_cache failed: {e}")
        return 0
