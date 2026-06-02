# -*- coding: utf-8 -*-
"""Finalized DPS encounter history storage.

This module intentionally stays off the parser/DPS hot path: callers feed it
already-finalized encounter reports, and it writes a compact rolling history
with atomic file replacement.
"""

import copy
import csv
import json
import os
import tempfile
import threading
import time
from typing import Any, Dict, List, Optional

from config import BASE_DIR

DPS_HISTORY_SCHEMA_VERSION = 1
DEFAULT_HISTORY_LIMIT = 100
DPS_HISTORY_EXPORT_DIR = os.path.join(BASE_DIR, "exports", "dps_history")
DPS_HISTORY_PATH = os.path.join(DPS_HISTORY_EXPORT_DIR, "history.json")


def _utc_now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _coerce_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


def _coerce_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _sanitize_entity(entity: Any) -> Dict[str, Any]:
    src = entity if isinstance(entity, dict) else {}
    return {
        "uid": _coerce_int(src.get("uid")),
        "name": str(src.get("name") or ""),
        "profession": str(src.get("profession") or ""),
        "damage_total": _coerce_int(src.get("damage_total")),
        "heal_total": _coerce_int(src.get("heal_total")),
        "damage_taken": _coerce_int(src.get("damage_taken")),
        "dps": _coerce_int(src.get("dps")),
        "hps": _coerce_int(src.get("hps")),
        "damage_pct": _coerce_float(src.get("damage_pct")),
        "is_self": bool(src.get("is_self")),
        "fight_point": _coerce_int(src.get("fight_point")),
    }


def _compact_report(report: Dict[str, Any]) -> Dict[str, Any]:
    entities = report.get("entities") if isinstance(report.get("entities"), list) else []
    return {
        "completed_at": _coerce_float(report.get("completed_at"), time.time()),
        "completed_local_time": str(report.get("completed_local_time") or ""),
        "report_reason": str(report.get("report_reason") or "completed"),
        "encounter_started_at": _coerce_float(report.get("encounter_started_at")),
        "encounter_ended_at": _coerce_float(report.get("encounter_ended_at")),
        "elapsed_s": _coerce_float(report.get("elapsed_s")),
        "total_damage": _coerce_int(report.get("total_damage")),
        "total_damage_all": _coerce_int(report.get("total_damage_all")),
        "total_heal": _coerce_int(report.get("total_heal")),
        "total_dps": _coerce_int(report.get("total_dps")),
        "total_hps": _coerce_int(report.get("total_hps")),
        "entities": [_sanitize_entity(entity) for entity in entities[:64]],
    }


def _safe_filename_part(value: Any, default: str = "encounter") -> str:
    text = str(value or default).strip().replace(" ", "_")
    out = []
    for ch in text:
        if ch.isalnum() or ch in ("-", "_"):
            out.append(ch)
        elif "\u4e00" <= ch <= "\u9fff":
            out.append(ch)
        else:
            out.append("_")
    cleaned = "".join(out).strip("_")
    return cleaned or default


class DpsHistoryStore:
    """Thread-safe rolling store for finalized DPS encounter summaries."""

    def __init__(self, path: str = DPS_HISTORY_PATH, limit: int = DEFAULT_HISTORY_LIMIT):
        self._path = path
        self._limit = max(1, int(limit or DEFAULT_HISTORY_LIMIT))
        self._lock = threading.RLock()
        self._items: List[Dict[str, Any]] = []
        self._load()

    @property
    def path(self) -> str:
        return self._path

    def _load(self) -> None:
        try:
            with open(self._path, "r", encoding="utf-8") as fp:
                data = json.load(fp)
        except FileNotFoundError:
            return
        except Exception:
            return
        items = data.get("items") if isinstance(data, dict) else data
        if not isinstance(items, list):
            return
        self._items = [copy.deepcopy(item) for item in items if isinstance(item, dict)][-self._limit:]

    def _save_locked(self) -> None:
        os.makedirs(os.path.dirname(self._path), exist_ok=True)
        payload = {
            "schema_version": DPS_HISTORY_SCHEMA_VERSION,
            "updated_at": _utc_now_iso(),
            "items": self._items[-self._limit:],
        }
        fd, tmp_path = tempfile.mkstemp(
            prefix="history.", suffix=".tmp", dir=os.path.dirname(self._path), text=True
        )
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fp:
                json.dump(payload, fp, ensure_ascii=False, separators=(",", ":"))
            os.replace(tmp_path, self._path)
        finally:
            try:
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except Exception:
                pass

    def add_report(self, report: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        if not isinstance(report, dict):
            return None
        item = _compact_report(report)
        with self._lock:
            self._items.append(item)
            self._items = self._items[-self._limit:]
            self._save_locked()
            return copy.deepcopy(item)

    def list_reports(self, limit: int = 20) -> List[Dict[str, Any]]:
        cap = max(1, min(int(limit or 20), self._limit))
        with self._lock:
            return copy.deepcopy(list(reversed(self._items[-cap:])))

    def latest_report(self) -> Optional[Dict[str, Any]]:
        with self._lock:
            if not self._items:
                return None
            return copy.deepcopy(self._items[-1])

    def export_report(self, report: Optional[Dict[str, Any]] = None,
                      fmt: str = "json") -> Optional[str]:
        src = report if isinstance(report, dict) else self.latest_report()
        if not isinstance(src, dict):
            return None
        item = _compact_report(src)
        fmt = str(fmt or "json").strip().lower()
        if fmt not in ("json", "csv"):
            fmt = "json"
        os.makedirs(DPS_HISTORY_EXPORT_DIR, exist_ok=True)
        stamp = time.strftime(
            "%Y%m%d_%H%M%S",
            time.localtime(_coerce_float(item.get("completed_at"), time.time())),
        )
        reason = _safe_filename_part(item.get("report_reason"), "encounter")
        path = os.path.join(DPS_HISTORY_EXPORT_DIR, f"dps_{stamp}_{reason}.{fmt}")
        if fmt == "csv":
            with open(path, "w", encoding="utf-8-sig", newline="") as fp:
                writer = csv.DictWriter(fp, fieldnames=[
                    "rank", "uid", "name", "profession", "damage_total", "dps",
                    "damage_pct", "heal_total", "hps", "damage_taken", "is_self", "fight_point",
                    "elapsed_s", "total_damage", "total_heal", "report_reason",
                ])
                writer.writeheader()
                entities = item.get("entities") if isinstance(item.get("entities"), list) else []
                for idx, entity in enumerate(entities, 1):
                    row = dict(entity)
                    row.update({
                        "rank": idx,
                        "elapsed_s": item.get("elapsed_s", 0),
                        "total_damage": item.get("total_damage", 0),
                        "total_heal": item.get("total_heal", 0),
                        "report_reason": item.get("report_reason", ""),
                    })
                    writer.writerow(row)
        else:
            with open(path, "w", encoding="utf-8") as fp:
                json.dump(item, fp, ensure_ascii=False, indent=2)
        return path

    def clear(self) -> None:
        with self._lock:
            self._items.clear()
            self._save_locked()


__all__ = [
    "DPS_HISTORY_EXPORT_DIR",
    "DPS_HISTORY_PATH",
    "DpsHistoryStore",
]
