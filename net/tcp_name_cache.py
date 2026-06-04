# -*- coding: utf-8 -*-
"""Runtime TCP name correspondence cache.

The cache records already decoded TCP/MEM facts (Chinese names, IDs and
server endpoints) for pre-parse lookup and later diagnostics. It deliberately
stores no raw packet payloads.
"""
from __future__ import annotations

import copy
import json
import os
import tempfile
import threading
import time
from typing import Any, Callable, Mapping, Optional

try:
    from packet_parser.skills import PROFESSION_NAMES  # type: ignore
except Exception:  # pragma: no cover - import fallback for standalone tools
    PROFESSION_NAMES = {}

_SCHEMA_VERSION = 1
_VALID_CONFIDENCE = {"high", "medium", "low", "mem", "tcp", "static"}
_GENERIC_KINDS = {
    "skill", "field_marker", "boss_skill", "ultimate_skill", "roguelike_affix",
    "profession_skill", "scripted_skill", "virtual_skill", "boss_mechanic_skill",
    "monster", "boss", "buff", "player_buff", "factor_buff", "profession_skill_buff", "event",
    "dungeon", "scene", "boss_mechanic", "npc", "item", "sub_profession",
}
_PLAYER_KIND = "player"

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(_HERE)
_DEFAULT_CACHE_PATH = os.path.join(_SAO, "assets", "name_tables", "tcp_preparse_name_cache.json")

_LIVE_ID_SPACE_KIND = {
    "skill_id": "skill",
    "skill_id_or_legacy_skill_name_index": "skill",
    "sub_profession_skill_id": "skill",
    "buff_id": "buff",
    "monster_id": "monster",
    "boss_id": "boss",
    "dungeon_id": "dungeon",
    "scene_id": "dungeon",
    "boss_event_type": "boss_mechanic",
    "npc_id": "npc",
    "item_id": "item",
}
_FALLBACK_LABEL_PREFIXES = {
    "技能", "场地标记", "Boss技能", "幻想技能", "肉鸽词条", "职业技能",
    "剧情表演", "虚拟体技能", "Boss机制技能", "怪物", "Boss", "地牢",
    "Buff", "玩家Buff", "因子Buff", "职业技能Buff", "事件", "机制", "NPC", "道具",
}


def _semantic_live_kind(id_space: str, id_: Any) -> str:
    fallback = _LIVE_ID_SPACE_KIND.get(id_space, "")
    if not fallback:
        return ""
    try:
        from tools.tablekit.name_table_classifier import classify_id
        classified = classify_id(id_space, id_)
        return classified or fallback
    except Exception:
        return fallback

_VOLATILE_RUNTIME_KEYS = {
    "obj", "chars", "klass", "runtime_klass", "string_klass", "string_obj",
    "string_chars", "slot_addr", "array_obj", "element_base",
    "table_element_base", "allLocalizationString_array_obj",
    "allLocalizationString_element_base",
}


def default_cache_path() -> str:
    return _DEFAULT_CACHE_PATH


def now_ts() -> float:
    return time.time()


def _iso(ts: Optional[float] = None) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(float(ts if ts is not None else now_ts())))


def _dotted_from_hex(endpoint_hex: str) -> str:
    host = str(endpoint_hex or "").split(":", 1)[0]
    if len(host) != 8:
        return ""
    try:
        return ".".join(str(int(host[i:i + 2], 16)) for i in range(0, 8, 2))
    except Exception:
        return ""


def _port_from_endpoint(endpoint: str) -> int:
    try:
        return int(str(endpoint or "").rsplit(":", 1)[1])
    except Exception:
        return 0


def _clean_text(text: Any) -> str:
    value = str(text or "").strip()
    if not value:
        return ""
    if "#" in value and value.split("#", 1)[0] in _FALLBACK_LABEL_PREFIXES:
        return ""
    return value


def _coerce_int(value: Any) -> int:
    try:
        return int(value)
    except Exception:
        return 0


def _merge_unique_list(old: Any, items: list[Any], *, limit: int = 16) -> list[Any]:
    out: list[Any] = []
    seen = set()
    for item in list(old or []) + list(items or []):
        key = json.dumps(item, ensure_ascii=False, sort_keys=True) if isinstance(item, (dict, list)) else str(item)
        if key in seen:
            continue
        seen.add(key)
        out.append(item)
        if len(out) >= limit:
            break
    return out


def _stable_context_from_live_row(row: Mapping[str, Any], match: Mapping[str, Any], id_space: str) -> dict[str, Any]:
    """Return stable preparse context without session-specific heap pointers."""
    runtime = row.get("runtime") if isinstance(row.get("runtime"), Mapping) else {}
    stable_runtime = {
        key: value
        for key, value in runtime.items()
        if key not in _VOLATILE_RUNTIME_KEYS
    }
    context = {
        "id_space": id_space,
        "tcp_field": match.get("tcp_field") or match.get("field") or "",
        "anchor_status": runtime.get("anchor_status") or row.get("anchor_status") or "",
    }
    if runtime.get("allLocalizationString_index") is not None:
        context["allLocalizationString_index"] = runtime.get("allLocalizationString_index")
    if stable_runtime:
        context["runtime_evidence"] = stable_runtime
    return context


def empty_snapshot() -> dict[str, Any]:
    return {
        "schema_version": _SCHEMA_VERSION,
        "updated_at": "",
        "endpoints": {},
        "names": {"by_kind": {}},
    }


def build_index_from_live_rows(rows_obj: Any, *, confidence: set[str] | None = None) -> dict[str, Any]:
    """Build a compact by-kind index from live_probe_act_matched_rows data."""
    accepted = confidence or {"high", "medium"}
    out = empty_snapshot()
    rows = rows_obj.get("rows") if isinstance(rows_obj, Mapping) else rows_obj
    for row in rows or []:
        if not isinstance(row, Mapping):
            continue
        row_conf = str(row.get("confidence") or "").strip().lower()
        if row_conf not in accepted:
            continue
        text = _clean_text(row.get("text"))
        if not text:
            continue
        match = row.get("primary_match") if isinstance(row.get("primary_match"), Mapping) else {}
        id_space = str(match.get("id_space") or "")
        iid = _coerce_int(match.get("id"))
        kind = _semantic_live_kind(id_space, iid)
        if not kind or iid <= 0:
            continue
        by_kind = out.setdefault("names", {}).setdefault("by_kind", {})
        bucket = by_kind.setdefault(kind, {})
        bucket[str(iid)] = {
            "text": text,
            "confidence": row_conf,
            "sources": [str(match.get("source") or match.get("source_kind") or "live_probe")],
            "endpoints": [],
            "updated_at": _iso(),
            "context": _stable_context_from_live_row(row, match, id_space),
        }
    out["updated_at"] = _iso()
    return out


class TcpNameCache:
    """Small JSON-backed correspondence cache for TCP/MEM decoded names."""

    def __init__(self, path: str | None = None, *, autosave_interval_s: float = 5.0,
                 max_entries_per_kind: int = 4096,
                 on_save: Callable[[str], None] | None = None) -> None:
        self.path = path or _DEFAULT_CACHE_PATH
        self.autosave_interval_s = max(0.0, float(autosave_interval_s or 0.0))
        self.max_entries_per_kind = max(64, int(max_entries_per_kind or 4096))
        self.on_save = on_save
        self._lock = threading.RLock()
        self._data: dict[str, Any] = empty_snapshot()
        self._dirty = False
        self._last_save_ts = 0.0
        self.load()

    def load(self) -> None:
        with self._lock:
            if not os.path.isfile(self.path):
                self._data = empty_snapshot()
                return
            try:
                with open(self.path, "r", encoding="utf-8") as f:
                    obj = json.load(f)
            except Exception:
                self._data = empty_snapshot()
                return
            if not isinstance(obj, dict):
                obj = empty_snapshot()
            obj.setdefault("schema_version", _SCHEMA_VERSION)
            obj.setdefault("updated_at", "")
            obj.setdefault("endpoints", {})
            obj.setdefault("names", {}).setdefault("by_kind", {})
            self._data = obj
            self._dirty = False

    def save(self, *, force: bool = False) -> bool:
        with self._lock:
            if not self._dirty and not force:
                return False
            now = now_ts()
            if not force and self.autosave_interval_s and now - self._last_save_ts < self.autosave_interval_s:
                return False
            data = copy.deepcopy(self._data)
            data["updated_at"] = _iso(now)
            folder = os.path.dirname(self.path)
            os.makedirs(folder, exist_ok=True)
            fd, tmp_path = tempfile.mkstemp(prefix="tcp_preparse_name_cache.", suffix=".tmp", dir=folder)
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
                    f.write("\n")
                os.replace(tmp_path, self.path)
            finally:
                try:
                    if os.path.exists(tmp_path):
                        os.unlink(tmp_path)
                except Exception:
                    pass
            self._data = data
            self._dirty = False
            self._last_save_ts = now
        if self.on_save:
            try:
                self.on_save(self.path)
            except Exception:
                pass
        return True

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._data)

    def observe_endpoint(self, endpoint_hex: str, endpoint_ip: str = "", endpoint_port: int = 0,
                         *, source: str = "tcp", timestamp: Optional[float] = None,
                         context: Mapping[str, Any] | None = None) -> None:
        endpoint_hex = str(endpoint_hex or "").strip()
        if not endpoint_hex:
            return
        ts = float(timestamp if timestamp is not None else now_ts())
        endpoint_ip = str(endpoint_ip or "").strip() or _dotted_from_hex(endpoint_hex)
        endpoint_port = int(endpoint_port or _port_from_endpoint(endpoint_hex) or 0)
        with self._lock:
            endpoints = self._data.setdefault("endpoints", {})
            entry = endpoints.setdefault(endpoint_hex, {})
            entry.setdefault("first_seen", _iso(ts))
            entry["last_seen"] = _iso(ts)
            entry["ip"] = endpoint_ip
            entry["port"] = endpoint_port
            entry["source"] = str(source or "tcp")
            entry["seen_count"] = int(entry.get("seen_count") or 0) + 1
            if context:
                entry["last_context"] = dict(context)
            self._dirty = True
        self.save()

    def observe_name(self, kind: str, id_: Any, text: Any, *, source: str = "tcp",
                     confidence: str = "medium", endpoint: str = "",
                     context: Mapping[str, Any] | None = None) -> None:
        kind = str(kind or "").strip()
        if kind not in _GENERIC_KINDS:
            return
        iid = _coerce_int(id_)
        if iid <= 0:
            return
        text_value = _clean_text(text)
        if not text_value:
            return
        conf = str(confidence or "medium").strip().lower()
        if conf not in _VALID_CONFIDENCE:
            conf = "medium"
        with self._lock:
            by_kind = self._data.setdefault("names", {}).setdefault("by_kind", {})
            bucket = by_kind.setdefault(kind, {})
            if len(bucket) >= self.max_entries_per_kind and str(iid) not in bucket:
                return
            entry = bucket.setdefault(str(iid), {})
            if not entry.get("text") or conf in {"high", "mem", "tcp", "static"}:
                entry["text"] = text_value
            entry["confidence"] = conf
            entry["updated_at"] = _iso()
            entry["sources"] = _merge_unique_list(entry.get("sources"), [str(source or "tcp")])
            if endpoint:
                entry["endpoints"] = _merge_unique_list(entry.get("endpoints"), [str(endpoint)], limit=8)
            else:
                entry.setdefault("endpoints", [])
            if context:
                entry["context"] = dict(context)
            self._dirty = True
        self.save()

    def observe_player(self, uid: Any, *, name: Any = "", level: Any = 0, hp: Any = 0, max_hp: Any = 0,
                       profession_id: Any = 0, profession_name: Any = "", fight_point: Any = 0,
                       skills: Any = None, source: str = "tcp", endpoint: str = "",
                       context: Mapping[str, Any] | None = None) -> None:
        uid_i = _coerce_int(uid)
        if uid_i <= 0:
            return
        prof_id = _coerce_int(profession_id)
        prof_name = _clean_text(profession_name) or (PROFESSION_NAMES.get(prof_id, "") if prof_id > 0 else "")
        skill_rows = []
        if isinstance(skills, Mapping):
            iterable = []
            for key, value in skills.items():
                if isinstance(value, Mapping):
                    row = dict(value)
                    row.setdefault("skill_id", key)
                    iterable.append(row)
                else:
                    iterable.append(key)
        else:
            iterable = skills or []
        for item in iterable:
            if isinstance(item, Mapping):
                sid = _coerce_int(item.get("skill_level_id") or item.get("skill_id") or item.get("id"))
                if sid > 0:
                    skill_rows.append({
                        "skill_level_id": sid,
                        "skill_id": _coerce_int(item.get("skill_id") or (sid // 100 if sid >= 100 else sid)),
                        "name": _clean_text(item.get("name")),
                    })
            else:
                sid = _coerce_int(item)
                if sid > 0:
                    skill_rows.append({"skill_level_id": sid, "skill_id": sid // 100 if sid >= 100 else sid})
        with self._lock:
            by_kind = self._data.setdefault("names", {}).setdefault("by_kind", {})
            players = by_kind.setdefault(_PLAYER_KIND, {})
            entry = players.setdefault(str(uid_i), {})
            name_text = _clean_text(name)
            if name_text:
                entry["name"] = name_text
                entry["text"] = name_text
            for key, value in (
                ("level", _coerce_int(level)),
                ("hp", _coerce_int(hp)),
                ("max_hp", _coerce_int(max_hp)),
                ("profession_id", prof_id),
                ("fight_point", _coerce_int(fight_point)),
            ):
                if value > 0 or key == "hp":
                    entry[key] = value
            if prof_name:
                entry["profession_name"] = prof_name
            if skill_rows:
                entry["skills"] = _merge_unique_list(entry.get("skills"), skill_rows, limit=64)
            entry["source"] = str(source or "tcp")
            entry["updated_at"] = _iso()
            if endpoint:
                entry["endpoints"] = _merge_unique_list(entry.get("endpoints"), [str(endpoint)], limit=8)
            else:
                entry.setdefault("endpoints", [])
            if context:
                entry["context"] = dict(context)
            self._dirty = True
        self.save()

    def apply_mem_self_snapshot(self, snapshot: Mapping[str, Any] | Any, *, endpoint: str = "",
                                context: Mapping[str, Any] | None = None) -> None:
        if snapshot is None:
            return
        getter = snapshot.get if isinstance(snapshot, Mapping) else lambda key, default=None: getattr(snapshot, key, default)
        uid = getter("uid", None) or getter("char_id", None) or getter("player_id", None)
        skills = getter("skill_cds", None) or getter("skill_slots", None) or []
        self.observe_player(
            uid,
            name=getter("char_name", None) or getter("name", None) or getter("player_name", ""),
            level=getter("level_base", None) or getter("level", 0),
            hp=getter("cur_hp", None) or getter("hp", None) or getter("hp_current", 0),
            max_hp=getter("max_hp", None) or getter("hp_max", 0),
            profession_id=getter("profession_id", 0),
            profession_name=getter("profession_name", ""),
            fight_point=getter("fight_point", 0),
            skills=skills,
            source="mem",
            endpoint=endpoint,
            context=context,
        )


__all__ = [
    "TcpNameCache",
    "build_index_from_live_rows",
    "default_cache_path",
    "empty_snapshot",
]
