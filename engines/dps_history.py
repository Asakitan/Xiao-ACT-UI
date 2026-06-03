# -*- coding: utf-8 -*-
"""Finalized DPS encounter history storage.

This module intentionally stays off the parser/DPS hot path: callers feed it
already-finalized encounter reports, and it writes a compact rolling history
with atomic file replacement.
"""

import copy
import csv
import html
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
        "encounter_id": str(report.get("encounter_id") or report.get("id") or ""),
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


def _history_search_text(item: Dict[str, Any]) -> str:
    pieces = [
        item.get("encounter_id"),
        item.get("completed_local_time"),
        item.get("report_reason"),
        item.get("total_damage"),
        item.get("total_heal"),
    ]
    entities = item.get("entities") if isinstance(item.get("entities"), list) else []
    for entity in entities:
        if isinstance(entity, dict):
            pieces.extend([
                entity.get("uid"),
                entity.get("name"),
                entity.get("profession"),
                entity.get("damage_total"),
                entity.get("heal_total"),
            ])
    return " ".join(str(part or "") for part in pieces).lower()


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


def _format_number(value: Any) -> str:
    try:
        return f"{int(value or 0):,}"
    except Exception:
        return "0"


def _format_pct(value: Any) -> str:
    try:
        num = float(value or 0.0)
    except Exception:
        num = 0.0
    if num <= 1.0:
        num *= 100.0
    return f"{num:.1f}%"


def _render_html_report(item: Dict[str, Any]) -> str:
    entities = item.get("entities") if isinstance(item.get("entities"), list) else []
    rows = []
    for idx, entity in enumerate(entities, 1):
        rows.append(
            "<tr>"
            f"<td>{idx}</td>"
            f"<td>{html.escape(str(entity.get('name') or ''))}</td>"
            f"<td>{html.escape(str(entity.get('profession') or ''))}</td>"
            f"<td>{_format_number(entity.get('damage_total'))}</td>"
            f"<td>{_format_number(entity.get('dps'))}</td>"
            f"<td>{_format_pct(entity.get('damage_pct'))}</td>"
            f"<td>{_format_number(entity.get('heal_total'))}</td>"
            f"<td>{_format_number(entity.get('damage_taken'))}</td>"
            "</tr>"
        )
    title = html.escape(str(item.get("encounter_id") or "ACT Encounter Report"))
    completed = html.escape(str(item.get("completed_local_time") or ""))
    reason = html.escape(str(item.get("report_reason") or ""))
    return """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>SAO Auto ACT Report</title>
  <style>
    body {{ font-family: Segoe UI, Arial, sans-serif; margin: 24px; color: #17202a; background: #f7f9fb; }}
    main {{ max-width: 1080px; margin: 0 auto; background: #fff; border: 1px solid #d9e2ec; padding: 20px; }}
    h1 {{ margin: 0 0 6px; font-size: 24px; }}
    .meta {{ color: #52616f; margin-bottom: 18px; }}
    .summary {{ display: grid; grid-template-columns: repeat(4, minmax(120px, 1fr)); gap: 10px; margin-bottom: 18px; }}
    .metric {{ border: 1px solid #d9e2ec; padding: 10px; background: #fbfdff; }}
    .metric b {{ display: block; font-size: 18px; margin-top: 4px; }}
    table {{ width: 100%; border-collapse: collapse; background: #fff; }}
    th, td {{ border-bottom: 1px solid #e6edf3; padding: 8px; text-align: right; }}
    th:nth-child(2), td:nth-child(2), th:nth-child(3), td:nth-child(3) {{ text-align: left; }}
    th {{ background: #eef4f8; font-weight: 600; }}
  </style>
</head>
<body>
<main>
  <h1>SAO Auto ACT Report</h1>
  <div class="meta">Encounter: {title} · Completed: {completed} · Reason: {reason}</div>
  <section class="summary">
    <div class="metric">Damage<b>{total_damage}</b></div>
    <div class="metric">Heal<b>{total_heal}</b></div>
    <div class="metric">DPS<b>{total_dps}</b></div>
    <div class="metric">Elapsed<b>{elapsed:.1f}s</b></div>
  </section>
  <table>
    <thead>
      <tr><th>#</th><th>Name</th><th>Profession</th><th>Damage</th><th>DPS</th><th>Damage %</th><th>Heal</th><th>Taken</th></tr>
    </thead>
    <tbody>
      {rows}
    </tbody>
  </table>
</main>
</body>
</html>
""".format(
        title=title,
        completed=completed,
        reason=reason,
        total_damage=_format_number(item.get("total_damage")),
        total_heal=_format_number(item.get("total_heal")),
        total_dps=_format_number(item.get("total_dps")),
        elapsed=_coerce_float(item.get("elapsed_s")),
        rows="\n      ".join(rows) or "<tr><td colspan=\"8\">No combatant rows</td></tr>",
    )


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

    def search_reports(self, query: str = "", limit: int = 20) -> List[Dict[str, Any]]:
        cap = max(1, min(int(limit or 20), self._limit))
        text = str(query or "").strip().lower()
        out: List[Dict[str, Any]] = []
        with self._lock:
            indexed = list(enumerate(self._items))
            for internal_index, item in reversed(indexed):
                if text and text not in _history_search_text(item):
                    continue
                row = copy.deepcopy(item)
                row["_history_index"] = len(self._items) - 1 - internal_index
                out.append(row)
                if len(out) >= cap:
                    break
        return out

    def _internal_index_locked(self, index: int, *, newest_first: bool = True) -> int:
        pos = int(index or 0)
        if pos < 0 or pos >= len(self._items):
            raise IndexError("history report index out of range")
        return len(self._items) - 1 - pos if newest_first else pos

    def get_report(self, index: int = 0, *, newest_first: bool = True) -> Optional[Dict[str, Any]]:
        with self._lock:
            if not self._items:
                return None
            internal_index = self._internal_index_locked(index, newest_first=newest_first)
            return copy.deepcopy(self._items[internal_index])

    def delete_report(self, index: int = 0, *, newest_first: bool = True) -> Optional[Dict[str, Any]]:
        with self._lock:
            if not self._items:
                return None
            internal_index = self._internal_index_locked(index, newest_first=newest_first)
            deleted = self._items.pop(internal_index)
            self._save_locked()
            return copy.deepcopy(deleted)

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
        if fmt not in ("json", "csv", "html"):
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
        elif fmt == "html":
            with open(path, "w", encoding="utf-8") as fp:
                fp.write(_render_html_report(item))
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
