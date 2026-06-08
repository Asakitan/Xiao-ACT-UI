# -*- coding: utf-8 -*-
"""Persisted aggregation of observed boss skills/mechanics, keyed scene → boss →
observation. Survives restarts so the reaction editor can browse every boss seen
on every map. Skill identity is the memory/TCP base_id (authoritative); names and
type tags are best-effort. Atomic writes + RLock mirror net.tcp_name_cache."""
from __future__ import annotations

import json
import os
import tempfile
import threading
import time
from typing import Any, Dict, List, Optional

SCHEMA_VERSION = 1

# Observation kinds.
KIND_SKILL = "skill"       # a cast skill (buff base_id appeared on the boss)
KIND_MECHANIC = "mechanic"  # a boss mechanic event (breaking/shield/fracture/death/part)
KIND_STATE = "state"       # a tracked state onset (enrage/invincible)

# How tight the HP%/time spread must be across occurrences to call it a line/timed cue.
_HP_LINE_SPREAD = 0.08
_TIME_FIXED_SPREAD = 3.0


def _default_store_path() -> str:
    env = os.environ.get("SAO_BOSS_SKILL_STORE")
    if env:
        return env
    try:
        import config  # frozen-aware: assets/ sits at BASE_DIR root when packaged
        return config.resource_path("assets", "name_tables", "boss_skill_observations.local.json")
    except Exception:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        return os.path.join(root, "boss_skill_observations.local.json")


def _now() -> float:
    return time.time()


class BossSkillStore:
    """Thread-safe, disk-backed scene→boss→observation aggregation."""

    def __init__(self, path: Optional[str] = None, autosave_interval: float = 5.0):
        self._lock = threading.RLock()
        self._path = path or _default_store_path()
        self._autosave = float(autosave_interval)
        self._dirty = False
        self._last_save = 0.0
        self._data: Dict[str, Any] = {"schema_version": SCHEMA_VERSION,
                                      "updated_at": 0.0, "scenes": {}}
        self.load()

    # ── persistence ──────────────────────────────────────────────────────────
    def load(self) -> None:
        with self._lock:
            try:
                with open(self._path, "r", encoding="utf-8") as fh:
                    data = json.load(fh)
                if isinstance(data, dict) and isinstance(data.get("scenes"), dict):
                    data.setdefault("schema_version", SCHEMA_VERSION)
                    data.setdefault("updated_at", 0.0)
                    self._data = data
            except (FileNotFoundError, ValueError, OSError):
                pass   # missing/corrupt → keep empty store

    def save(self, force: bool = False) -> bool:
        with self._lock:
            if not (self._dirty or force):
                return False
            self._data["updated_at"] = _now()
            payload = json.dumps(self._data, ensure_ascii=False, indent=1)
            try:
                d = os.path.dirname(self._path)
                if d and not os.path.isdir(d):
                    os.makedirs(d, exist_ok=True)
                fd, tmp = tempfile.mkstemp(dir=d or None, suffix=".tmp")
                try:
                    with os.fdopen(fd, "w", encoding="utf-8") as fh:
                        fh.write(payload)
                    os.replace(tmp, self._path)
                finally:
                    if os.path.exists(tmp):
                        os.remove(tmp)
            except OSError:
                return False
            self._dirty = False
            self._last_save = _now()
            return True

    def maybe_save(self) -> bool:
        with self._lock:
            if self._dirty and (_now() - self._last_save) >= self._autosave:
                return self.save()
            return False

    # ── recording ────────────────────────────────────────────────────────────
    def observe(self, *, scene_key: str, scene_id: int = 0, dungeon_id: int = 0,
                scene_name: str = "", boss_base_id: int, boss_name: str = "",
                obs_id: int, name: str = "", kind: str = KIND_SKILL,
                tags: Optional[List[str]] = None, duration_ms: Optional[int] = None,
                hp_pct: Optional[float] = None, elapsed_s: Optional[float] = None,
                max_hp: int = 0) -> None:
        """Upsert one observation. Counts repeats, unions tags, tracks HP%/time
        spread so the editor can mark blood-line / timed cues."""
        scene_key = str(scene_key or "0")
        with self._lock:
            scenes = self._data.setdefault("scenes", {})
            scene = scenes.get(scene_key)
            if scene is None:
                scene = {"scene_key": scene_key, "scene_id": int(scene_id or 0),
                         "dungeon_id": int(dungeon_id or 0), "name": scene_name or "",
                         "last_seen": 0.0, "bosses": {}}
                scenes[scene_key] = scene
            if scene_name and not scene.get("name"):
                scene["name"] = scene_name
            if scene_id and not scene.get("scene_id"):
                scene["scene_id"] = int(scene_id)
            if dungeon_id and not scene.get("dungeon_id"):
                scene["dungeon_id"] = int(dungeon_id)
            scene["last_seen"] = _now()

            bkey = str(int(boss_base_id or 0))
            bosses = scene.setdefault("bosses", {})
            boss = bosses.get(bkey)
            if boss is None:
                boss = {"base_id": int(boss_base_id or 0), "name": boss_name or "",
                        "max_hp": int(max_hp or 0), "last_seen": 0.0, "obs": {}}
                bosses[bkey] = boss
            if boss_name and not boss.get("name"):
                boss["name"] = boss_name
            if max_hp and max_hp > int(boss.get("max_hp") or 0):
                boss["max_hp"] = int(max_hp)
            boss["last_seen"] = _now()

            okey = "%s:%d" % (kind, int(obs_id or 0))
            obs = boss["obs"].get(okey)
            if obs is None:
                obs = {"id": int(obs_id or 0), "kind": kind, "name": name or "",
                       "count": 0, "tags": [], "last_cast_duration_ms": None,
                       "hp_pct": None, "hp_pct_min": None, "hp_pct_max": None,
                       "elapsed_s": None, "elapsed_min": None, "elapsed_max": None,
                       "first_ts": _now(), "last_ts": 0.0}
                boss["obs"][okey] = obs
            obs["count"] = int(obs.get("count") or 0) + 1
            obs["last_ts"] = _now()
            if name and not obs.get("name"):
                obs["name"] = name
            if tags:
                merged = list(obs.get("tags") or [])
                for t in tags:
                    if t and t not in merged:
                        merged.append(t)
                obs["tags"] = merged
            if isinstance(duration_ms, (int, float)) and duration_ms > 0:
                obs["last_cast_duration_ms"] = int(duration_ms)
            if isinstance(hp_pct, (int, float)):
                hp = float(hp_pct)
                obs["hp_pct"] = hp
                obs["hp_pct_min"] = hp if obs.get("hp_pct_min") is None else min(obs["hp_pct_min"], hp)
                obs["hp_pct_max"] = hp if obs.get("hp_pct_max") is None else max(obs["hp_pct_max"], hp)
            if isinstance(elapsed_s, (int, float)) and elapsed_s >= 0:
                el = float(elapsed_s)
                obs["elapsed_s"] = el
                obs["elapsed_min"] = el if obs.get("elapsed_min") is None else min(obs["elapsed_min"], el)
                obs["elapsed_max"] = el if obs.get("elapsed_max") is None else max(obs["elapsed_max"], el)
            self._dirty = True

    # ── queries (editor data source) ──────────────────────────────────────────
    def scenes(self) -> List[Dict[str, Any]]:
        with self._lock:
            out = []
            for sk, sc in self._data.get("scenes", {}).items():
                out.append({"scene_key": sk, "scene_id": int(sc.get("scene_id") or 0),
                            "dungeon_id": int(sc.get("dungeon_id") or 0),
                            "name": sc.get("name") or "",
                            "boss_count": len(sc.get("bosses") or {}),
                            "last_seen": float(sc.get("last_seen") or 0.0)})
            return sorted(out, key=lambda s: -s["last_seen"])

    def bosses(self, scene_key: Optional[str] = None) -> List[Dict[str, Any]]:
        with self._lock:
            out = []
            scenes = self._data.get("scenes", {})
            items = ([(str(scene_key), scenes.get(str(scene_key)))] if scene_key is not None
                     else list(scenes.items()))
            for sk, sc in items:
                if not sc:
                    continue
                for bk, boss in (sc.get("bosses") or {}).items():
                    cnt = sum(int(o.get("count") or 0) for o in (boss.get("obs") or {}).values())
                    out.append({"base_id": int(boss.get("base_id") or 0),
                                "name": boss.get("name") or "", "scene_key": sk,
                                "observed_count": cnt,
                                "last_seen": float(boss.get("last_seen") or 0.0)})
            return sorted(out, key=lambda b: -b["last_seen"])

    def observations(self, scene_key: Optional[str], boss_base_id: int) -> List[Dict[str, Any]]:
        """Display-ready observations for one boss (optionally scoped to a scene),
        with derived blood-line / timed tags merged in. Sorted mechanics/states
        first, then by count."""
        bkey = str(int(boss_base_id or 0))
        with self._lock:
            scenes = self._data.get("scenes", {})
            sks = [str(scene_key)] if scene_key is not None else list(scenes.keys())
            merged: Dict[str, Dict[str, Any]] = {}
            for sk in sks:
                sc = scenes.get(sk)
                if not sc:
                    continue
                boss = (sc.get("bosses") or {}).get(bkey)
                if not boss:
                    continue
                for okey, o in (boss.get("obs") or {}).items():
                    m = merged.get(okey)
                    if m is None:
                        merged[okey] = dict(o)
                    else:
                        m["count"] = int(m.get("count") or 0) + int(o.get("count") or 0)
                        m["tags"] = list(dict.fromkeys((m.get("tags") or []) + (o.get("tags") or [])))
                        if (o.get("last_ts") or 0) > (m.get("last_ts") or 0):
                            m["last_ts"] = o.get("last_ts")
                            m["last_cast_duration_ms"] = o.get("last_cast_duration_ms") or m.get("last_cast_duration_ms")
            return sorted([self._decorate(o) for o in merged.values()],
                          key=lambda r: (0 if r["kind"] != KIND_SKILL else 1, -r["count"]))

    @staticmethod
    def _decorate(o: Dict[str, Any]) -> Dict[str, Any]:
        rec = dict(o)
        rec["skill_id"] = int(rec.get("id") or 0)   # alias for reaction matching
        tags = list(rec.get("tags") or [])
        cnt = int(rec.get("count") or 0)
        lo, hi = rec.get("hp_pct_min"), rec.get("hp_pct_max")
        if cnt >= 2 and isinstance(lo, (int, float)) and isinstance(hi, (int, float)) \
                and 0.02 <= lo <= 0.98 and (hi - lo) <= _HP_LINE_SPREAD:
            if "hp_line" not in tags:
                tags.append("hp_line")
            rec["hp_line_pct"] = round((lo + hi) / 2.0, 4)
        elo, ehi = rec.get("elapsed_min"), rec.get("elapsed_max")
        if cnt >= 2 and isinstance(elo, (int, float)) and isinstance(ehi, (int, float)) \
                and elo >= 1.0 and (ehi - elo) <= _TIME_FIXED_SPREAD:
            if "time" not in tags:
                tags.append("time")
            rec["time_fixed_s"] = round((elo + ehi) / 2.0, 1)
        rec["tags"] = tags
        return rec
