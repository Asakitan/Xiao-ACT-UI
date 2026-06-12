# -*- coding: utf-8 -*-
"""Finalized DPS encounter history storage.

This module intentionally stays off the parser/DPS hot path: callers feed it
already-finalized encounter reports, and it writes a compact rolling history
with atomic file replacement.
"""

import copy
import csv
import gzip
import html
import json
import os
import sqlite3
import tempfile
import threading
import time
import zipfile
from typing import Any, Dict, List, Optional
from xml.etree import ElementTree as ET

from config import BASE_DIR

DPS_HISTORY_SCHEMA_VERSION = 1
DPS_HISTORY_JSONL_SCHEMA_VERSION = 1
DPS_HISTORY_SQLITE_SCHEMA_VERSION = 2
DEFAULT_HISTORY_LIMIT = 100
DPS_HISTORY_EXPORT_DIR = os.path.join(BASE_DIR, "exports", "dps_history")
DPS_HISTORY_PATH = os.path.join(DPS_HISTORY_EXPORT_DIR, "history.json")
DPS_HISTORY_JSONL_PATH = os.path.join(DPS_HISTORY_EXPORT_DIR, "history.jsonl")
DPS_HISTORY_SQLITE_PATH = os.path.join(DPS_HISTORY_EXPORT_DIR, "history.sqlite3")


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


def _json_text(value: Any) -> str:
    try:
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"), default=str)
    except Exception:
        return json.dumps(str(value), ensure_ascii=False)


def _list_from_report(report: Dict[str, Any], *keys: str) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for key in keys:
        items = report.get(key)
        if not isinstance(items, list):
            continue
        for item in items:
            if isinstance(item, dict):
                out.append(copy.deepcopy(item))
    return out


def _event_payload(event: Dict[str, Any]) -> Dict[str, Any]:
    payload = event.get("payload")
    return dict(payload) if isinstance(payload, dict) else event


def _event_topic(event: Dict[str, Any], default: str = "event") -> str:
    payload = _event_payload(event)
    return str(event.get("topic") or payload.get("topic") or payload.get("type") or default)


def _event_observed_at(event: Dict[str, Any]) -> float:
    payload = _event_payload(event)
    for key in ("observed_at", "timestamp", "time", "created_at"):
        if key in event:
            return _coerce_float(event.get(key))
        if key in payload:
            return _coerce_float(payload.get(key))
    time_ms = event.get("time_ms", payload.get("time_ms"))
    if time_ms is not None:
        return _coerce_float(time_ms) / 1000.0
    return 0.0


def _event_time_ms(event: Dict[str, Any]) -> int:
    payload = _event_payload(event)
    for key in ("time_ms", "cursor_ms", "offset_ms"):
        if key in event:
            return _coerce_int(event.get(key))
        if key in payload:
            return _coerce_int(payload.get(key))
    observed_at = _event_observed_at(event)
    return int(observed_at * 1000) if observed_at > 0 else 0


def _event_value(payload: Dict[str, Any]) -> float:
    for key in ("value", "damage", "damage_total", "heal", "heal_total", "amount"):
        if key in payload:
            return _coerce_float(payload.get(key))
    return 0.0


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


def _xml_child(parent: ET.Element, name: str, value: Any) -> ET.Element:
    child = ET.SubElement(parent, name)
    child.text = str(value if value is not None else "")
    return child


def _render_xml_report(item: Dict[str, Any]) -> str:
    root = ET.Element("sao_act_report", {"schema_version": str(DPS_HISTORY_SCHEMA_VERSION)})
    encounter = ET.SubElement(root, "encounter")
    for key in (
        "encounter_id",
        "completed_at",
        "completed_local_time",
        "report_reason",
        "encounter_started_at",
        "encounter_ended_at",
        "elapsed_s",
    ):
        _xml_child(encounter, key, item.get(key))
    totals = ET.SubElement(root, "totals")
    for key in ("total_damage", "total_damage_all", "total_heal", "total_dps", "total_hps"):
        _xml_child(totals, key, item.get(key))
    combatants = ET.SubElement(root, "combatants")
    entities = item.get("entities") if isinstance(item.get("entities"), list) else []
    for rank, entity in enumerate(entities, 1):
        if not isinstance(entity, dict):
            continue
        row = ET.SubElement(combatants, "combatant", {"rank": str(rank)})
        for key in (
            "uid",
            "name",
            "profession",
            "damage_total",
            "heal_total",
            "damage_taken",
            "dps",
            "hps",
            "damage_pct",
            "is_self",
            "fight_point",
        ):
            _xml_child(row, key, entity.get(key))
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + ET.tostring(root, encoding="unicode")


def _xml_find_text(parent: Optional[ET.Element], name: str, default: str = "") -> str:
    if parent is None:
        return default
    text = parent.findtext(name)
    return str(text if text is not None else default)


def _coerce_bool_text(value: Any) -> bool:
    text = str(value or "").strip().lower()
    return text in ("1", "true", "yes", "on")


def load_exported_xml_report(path: str) -> Dict[str, Any]:
    tree = ET.parse(path)
    root = tree.getroot()
    return _parse_exported_xml_root(root)


def _parse_exported_xml_text(text: str) -> Dict[str, Any]:
    root = ET.fromstring(text)
    return _parse_exported_xml_root(root)


def _parse_exported_xml_root(root: ET.Element) -> Dict[str, Any]:
    if root.tag != "sao_act_report":
        raise ValueError(f"unsupported XML report root: {root.tag}")
    encounter = root.find("encounter")
    totals = root.find("totals")
    entities = []
    for node in root.findall("combatants/combatant"):
        entities.append({
            "uid": _coerce_int(_xml_find_text(node, "uid")),
            "name": _xml_find_text(node, "name"),
            "profession": _xml_find_text(node, "profession"),
            "damage_total": _coerce_int(_xml_find_text(node, "damage_total")),
            "heal_total": _coerce_int(_xml_find_text(node, "heal_total")),
            "damage_taken": _coerce_int(_xml_find_text(node, "damage_taken")),
            "dps": _coerce_int(_xml_find_text(node, "dps")),
            "hps": _coerce_int(_xml_find_text(node, "hps")),
            "damage_pct": _coerce_float(_xml_find_text(node, "damage_pct")),
            "is_self": _coerce_bool_text(_xml_find_text(node, "is_self")),
            "fight_point": _coerce_int(_xml_find_text(node, "fight_point")),
        })
    return {
        "encounter_id": _xml_find_text(encounter, "encounter_id"),
        "completed_at": _coerce_float(_xml_find_text(encounter, "completed_at"), time.time()),
        "completed_local_time": _xml_find_text(encounter, "completed_local_time"),
        "report_reason": _xml_find_text(encounter, "report_reason", "xml_import"),
        "encounter_started_at": _coerce_float(_xml_find_text(encounter, "encounter_started_at")),
        "encounter_ended_at": _coerce_float(_xml_find_text(encounter, "encounter_ended_at")),
        "elapsed_s": _coerce_float(_xml_find_text(encounter, "elapsed_s")),
        "total_damage": _coerce_int(_xml_find_text(totals, "total_damage")),
        "total_damage_all": _coerce_int(_xml_find_text(totals, "total_damage_all")),
        "total_heal": _coerce_int(_xml_find_text(totals, "total_heal")),
        "total_dps": _coerce_int(_xml_find_text(totals, "total_dps")),
        "total_hps": _coerce_int(_xml_find_text(totals, "total_hps")),
        "entities": entities,
    }


def load_exported_report_file(path: str) -> Dict[str, Any]:
    text_path = str(path or "")
    lower_path = text_path.lower()
    suffix = os.path.splitext(text_path)[1].lower()
    if suffix == ".xml":
        return load_exported_xml_report(path)
    if lower_path.endswith(".xml.gz"):
        with gzip.open(text_path, "rt", encoding="utf-8") as fp:
            return _parse_exported_xml_text(fp.read())
    if lower_path.endswith(".xml.zip") or suffix == ".zip":
        with zipfile.ZipFile(text_path, "r") as zf:
            names = [name for name in zf.namelist() if not name.endswith("/")]
            xml_names = [name for name in names if name.lower().endswith(".xml")]
            if not xml_names:
                raise ValueError("compressed XML report does not contain an XML file")
            return _parse_exported_xml_text(zf.read(xml_names[0]).decode("utf-8"))
    raise ValueError(f"unsupported exported report import format: {suffix or path}")


class DpsHistoryStore:
    """Thread-safe rolling store for finalized DPS encounter summaries."""

    def __init__(self, path: str = DPS_HISTORY_PATH, limit: int = DEFAULT_HISTORY_LIMIT,
                 archive_path: Optional[str] = None,
                 sqlite_path: Optional[str] = None):
        self._path = path
        self._archive_path = (
            str(archive_path)
            if archive_path is not None
            else os.path.splitext(str(path or DPS_HISTORY_PATH))[0] + ".jsonl"
        )
        self._sqlite_path = (
            str(sqlite_path)
            if sqlite_path is not None
            else os.path.splitext(str(path or DPS_HISTORY_PATH))[0] + ".sqlite3"
        )
        self._limit = max(1, int(limit or DEFAULT_HISTORY_LIMIT))
        self._lock = threading.RLock()
        self._items: List[Dict[str, Any]] = []
        self._last_archive_error = ""
        self._last_sqlite_error = ""
        # sqlite_status 缓存: 写代失效 (本进程是唯一写者) — 每次 UI 轮询
        # 都跑 6 个 COUNT(*) 全表扫 + ensure_schema 不可取
        self._sqlite_write_gen = 0
        self._sqlite_status_cache: Optional[tuple] = None
        self._load()

    @property
    def path(self) -> str:
        return self._path

    @property
    def archive_path(self) -> str:
        return self._archive_path

    @property
    def sqlite_path(self) -> str:
        return self._sqlite_path

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

    def _append_archive_locked(self, item: Dict[str, Any]) -> None:
        if not self._archive_path:
            return
        try:
            archive_dir = os.path.dirname(self._archive_path)
            if archive_dir:
                os.makedirs(archive_dir, exist_ok=True)
            record = {
                "schema_version": DPS_HISTORY_JSONL_SCHEMA_VERSION,
                "archived_at": _utc_now_iso(),
                "report": copy.deepcopy(item),
            }
            with open(self._archive_path, "a", encoding="utf-8") as fp:
                fp.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
            self._last_archive_error = ""
        except Exception as exc:
            self._last_archive_error = str(exc)

    def _sqlite_connect_locked(self):
        if not self._sqlite_path:
            return None
        sqlite_dir = os.path.dirname(self._sqlite_path)
        if sqlite_dir:
            os.makedirs(sqlite_dir, exist_ok=True)
        conn = sqlite3.connect(self._sqlite_path)
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        return conn

    def _ensure_sqlite_schema_locked(self, conn) -> None:
        conn.execute(
            "CREATE TABLE IF NOT EXISTS meta ("
            "key TEXT PRIMARY KEY, "
            "value TEXT NOT NULL"
            ")"
        )
        conn.execute(
            "CREATE TABLE IF NOT EXISTS encounters ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "encounter_id TEXT NOT NULL, "
            "completed_at REAL NOT NULL, "
            "completed_local_time TEXT NOT NULL, "
            "report_reason TEXT NOT NULL, "
            "encounter_started_at REAL NOT NULL, "
            "encounter_ended_at REAL NOT NULL, "
            "elapsed_s REAL NOT NULL, "
            "total_damage INTEGER NOT NULL, "
            "total_damage_all INTEGER NOT NULL, "
            "total_heal INTEGER NOT NULL, "
            "total_dps INTEGER NOT NULL, "
            "total_hps INTEGER NOT NULL, "
            "payload_json TEXT NOT NULL, "
            "archived_at TEXT NOT NULL"
            ")"
        )
        conn.execute(
            "CREATE TABLE IF NOT EXISTS combatants ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "encounter_row_id INTEGER NOT NULL, "
            "rank INTEGER NOT NULL, "
            "uid INTEGER NOT NULL, "
            "name TEXT NOT NULL, "
            "profession TEXT NOT NULL, "
            "damage_total INTEGER NOT NULL, "
            "heal_total INTEGER NOT NULL, "
            "damage_taken INTEGER NOT NULL, "
            "dps INTEGER NOT NULL, "
            "hps INTEGER NOT NULL, "
            "damage_pct REAL NOT NULL, "
            "is_self INTEGER NOT NULL, "
            "fight_point INTEGER NOT NULL, "
            "FOREIGN KEY(encounter_row_id) REFERENCES encounters(id) ON DELETE CASCADE"
            ")"
        )
        conn.execute(
            "CREATE TABLE IF NOT EXISTS actions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "encounter_row_id INTEGER NOT NULL, "
            "seq INTEGER NOT NULL, "
            "topic TEXT NOT NULL, "
            "action_type TEXT NOT NULL, "
            "observed_at REAL NOT NULL, "
            "time_ms INTEGER NOT NULL, "
            "actor_uid INTEGER NOT NULL, "
            "actor_name TEXT NOT NULL, "
            "target_uid INTEGER NOT NULL, "
            "target_name TEXT NOT NULL, "
            "skill_id TEXT NOT NULL, "
            "skill_name TEXT NOT NULL, "
            "value REAL NOT NULL, "
            "source_name TEXT NOT NULL, "
            "source_kind TEXT NOT NULL, "
            "payload_json TEXT NOT NULL, "
            "FOREIGN KEY(encounter_row_id) REFERENCES encounters(id) ON DELETE CASCADE"
            ")"
        )
        conn.execute(
            "CREATE TABLE IF NOT EXISTS timeline_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "encounter_row_id INTEGER NOT NULL, "
            "seq INTEGER NOT NULL, "
            "event_type TEXT NOT NULL, "
            "observed_at REAL NOT NULL, "
            "time_ms INTEGER NOT NULL, "
            "label TEXT NOT NULL, "
            "payload_json TEXT NOT NULL, "
            "FOREIGN KEY(encounter_row_id) REFERENCES encounters(id) ON DELETE CASCADE"
            ")"
        )
        conn.execute(
            "CREATE TABLE IF NOT EXISTS trigger_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "encounter_row_id INTEGER NOT NULL, "
            "seq INTEGER NOT NULL, "
            "rule_id TEXT NOT NULL, "
            "severity TEXT NOT NULL, "
            "message TEXT NOT NULL, "
            "observed_at REAL NOT NULL, "
            "time_ms INTEGER NOT NULL, "
            "payload_json TEXT NOT NULL, "
            "FOREIGN KEY(encounter_row_id) REFERENCES encounters(id) ON DELETE CASCADE"
            ")"
        )
        conn.execute(
            "CREATE TABLE IF NOT EXISTS source_metadata ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "encounter_row_id INTEGER NOT NULL, "
            "key TEXT NOT NULL, "
            "value_json TEXT NOT NULL, "
            "FOREIGN KEY(encounter_row_id) REFERENCES encounters(id) ON DELETE CASCADE"
            ")"
        )
        conn.execute("CREATE INDEX IF NOT EXISTS idx_encounters_completed_at ON encounters(completed_at DESC)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_encounters_reason ON encounters(report_reason)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_combatants_encounter ON combatants(encounter_row_id, rank)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_combatants_name ON combatants(name)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_actions_encounter_time ON actions(encounter_row_id, time_ms, seq)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_actions_topic ON actions(topic)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_timeline_encounter_time ON timeline_events(encounter_row_id, time_ms, seq)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_trigger_encounter_time ON trigger_events(encounter_row_id, time_ms, seq)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_source_metadata_encounter ON source_metadata(encounter_row_id, key)")
        conn.execute(
            "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)",
            ("schema_version", str(DPS_HISTORY_SQLITE_SCHEMA_VERSION)),
        )

    def _append_sqlite_action_rows_locked(self, conn, encounter_row_id: int, report: Dict[str, Any]) -> None:
        events = _list_from_report(report, "actions", "events", "action_log")
        for seq, event in enumerate(events, 1):
            payload = _event_payload(event)
            conn.execute(
                "INSERT INTO actions("
                "encounter_row_id, seq, topic, action_type, observed_at, time_ms, "
                "actor_uid, actor_name, target_uid, target_name, skill_id, skill_name, "
                "value, source_name, source_kind, payload_json"
                ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    encounter_row_id,
                    seq,
                    _event_topic(event, "action"),
                    str(payload.get("action_type") or payload.get("kind") or payload.get("type") or _event_topic(event, "action")),
                    _event_observed_at(event),
                    _event_time_ms(event),
                    _coerce_int(payload.get("actor_uid") or payload.get("source_uid") or payload.get("caster_uid")),
                    str(payload.get("actor_name") or payload.get("source_name") or payload.get("caster_name") or ""),
                    _coerce_int(payload.get("target_uid") or payload.get("target_id")),
                    str(payload.get("target_name") or ""),
                    str(payload.get("skill_id") or payload.get("skill") or ""),
                    str(payload.get("skill_name") or payload.get("name") or ""),
                    _event_value(payload),
                    str(event.get("source_name") or payload.get("source_name") or ""),
                    str(event.get("source_kind") or payload.get("source_kind") or ""),
                    _json_text(event),
                ),
            )

    def _append_sqlite_timeline_rows_locked(self, conn, encounter_row_id: int, report: Dict[str, Any]) -> None:
        events = _list_from_report(report, "timeline", "timeline_events")
        for seq, event in enumerate(events, 1):
            payload = _event_payload(event)
            conn.execute(
                "INSERT INTO timeline_events("
                "encounter_row_id, seq, event_type, observed_at, time_ms, label, payload_json"
                ") VALUES (?, ?, ?, ?, ?, ?, ?)",
                (
                    encounter_row_id,
                    seq,
                    str(payload.get("event_type") or payload.get("type") or _event_topic(event, "timeline")),
                    _event_observed_at(event),
                    _event_time_ms(event),
                    str(payload.get("label") or payload.get("message") or payload.get("name") or ""),
                    _json_text(event),
                ),
            )

    def _append_sqlite_trigger_rows_locked(self, conn, encounter_row_id: int, report: Dict[str, Any]) -> None:
        events = _list_from_report(report, "trigger_events")
        for seq, event in enumerate(events, 1):
            payload = _event_payload(event)
            conn.execute(
                "INSERT INTO trigger_events("
                "encounter_row_id, seq, rule_id, severity, message, observed_at, time_ms, payload_json"
                ") VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    encounter_row_id,
                    seq,
                    str(payload.get("rule_id") or payload.get("id") or ""),
                    str(payload.get("severity") or payload.get("level") or ""),
                    str(payload.get("message") or payload.get("text") or ""),
                    _event_observed_at(event),
                    _event_time_ms(event),
                    _json_text(event),
                ),
            )

    def _append_sqlite_source_metadata_locked(self, conn, encounter_row_id: int, report: Dict[str, Any]) -> None:
        metadata = report.get("source_metadata")
        if not isinstance(metadata, dict):
            metadata = report.get("source")
        if not isinstance(metadata, dict):
            metadata = report.get("metadata")
        if not isinstance(metadata, dict):
            return
        for key, value in metadata.items():
            conn.execute(
                "INSERT INTO source_metadata(encounter_row_id, key, value_json) VALUES (?, ?, ?)",
                (encounter_row_id, str(key), _json_text(value)),
            )

    def _append_sqlite_locked(self, item: Dict[str, Any], raw_report: Optional[Dict[str, Any]] = None) -> None:
        if not self._sqlite_path:
            return
        conn = None
        try:
            conn = self._sqlite_connect_locked()
            if conn is None:
                return
            self._ensure_sqlite_schema_locked(conn)
            archived_at = _utc_now_iso()
            cur = conn.execute(
                "INSERT INTO encounters("
                "encounter_id, completed_at, completed_local_time, report_reason, "
                "encounter_started_at, encounter_ended_at, elapsed_s, total_damage, "
                "total_damage_all, total_heal, total_dps, total_hps, payload_json, archived_at"
                ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    str(item.get("encounter_id") or ""),
                    _coerce_float(item.get("completed_at")),
                    str(item.get("completed_local_time") or ""),
                    str(item.get("report_reason") or ""),
                    _coerce_float(item.get("encounter_started_at")),
                    _coerce_float(item.get("encounter_ended_at")),
                    _coerce_float(item.get("elapsed_s")),
                    _coerce_int(item.get("total_damage")),
                    _coerce_int(item.get("total_damage_all")),
                    _coerce_int(item.get("total_heal")),
                    _coerce_int(item.get("total_dps")),
                    _coerce_int(item.get("total_hps")),
                    json.dumps(item, ensure_ascii=False, separators=(",", ":")),
                    archived_at,
                ),
            )
            encounter_row_id = int(cur.lastrowid or 0)
            entities = item.get("entities") if isinstance(item.get("entities"), list) else []
            for rank, entity in enumerate(entities, 1):
                if not isinstance(entity, dict):
                    continue
                conn.execute(
                    "INSERT INTO combatants("
                    "encounter_row_id, rank, uid, name, profession, damage_total, heal_total, "
                    "damage_taken, dps, hps, damage_pct, is_self, fight_point"
                    ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        encounter_row_id,
                        rank,
                        _coerce_int(entity.get("uid")),
                        str(entity.get("name") or ""),
                        str(entity.get("profession") or ""),
                        _coerce_int(entity.get("damage_total")),
                        _coerce_int(entity.get("heal_total")),
                        _coerce_int(entity.get("damage_taken")),
                        _coerce_int(entity.get("dps")),
                        _coerce_int(entity.get("hps")),
                        _coerce_float(entity.get("damage_pct")),
                        1 if entity.get("is_self") else 0,
                        _coerce_int(entity.get("fight_point")),
                    ),
                )
            raw = raw_report if isinstance(raw_report, dict) else item
            self._append_sqlite_action_rows_locked(conn, encounter_row_id, raw)
            self._append_sqlite_timeline_rows_locked(conn, encounter_row_id, raw)
            self._append_sqlite_trigger_rows_locked(conn, encounter_row_id, raw)
            self._append_sqlite_source_metadata_locked(conn, encounter_row_id, raw)
            conn.commit()
            self._last_sqlite_error = ""
            self._sqlite_write_gen += 1
        except Exception as exc:
            self._last_sqlite_error = str(exc)
            self._sqlite_write_gen += 1
            try:
                if conn is not None:
                    conn.rollback()
            except Exception:
                pass
        finally:
            try:
                if conn is not None:
                    conn.close()
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
            self._append_archive_locked(item)
            self._append_sqlite_locked(item, raw_report=report)
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

    def list_archive_reports(self, limit: int = 100) -> List[Dict[str, Any]]:
        cap = max(1, int(limit or 100))
        if not self._archive_path or not os.path.isfile(self._archive_path):
            return []
        rows: List[Dict[str, Any]] = []
        try:
            with open(self._archive_path, "r", encoding="utf-8") as fp:
                for line in fp:
                    text = line.strip()
                    if not text:
                        continue
                    try:
                        record = json.loads(text)
                    except Exception:
                        continue
                    report = record.get("report") if isinstance(record, dict) else None
                    if isinstance(report, dict):
                        item = copy.deepcopy(report)
                        item.setdefault("_archive_schema_version", _coerce_int(record.get("schema_version")))
                        item.setdefault("_archived_at", str(record.get("archived_at") or ""))
                        rows.append(item)
        except Exception as exc:
            self._last_archive_error = str(exc)
            return []
        return list(reversed(rows[-cap:]))

    def archive_status(self) -> Dict[str, Any]:
        count = 0
        exists = bool(self._archive_path and os.path.isfile(self._archive_path))
        if exists:
            try:
                with open(self._archive_path, "r", encoding="utf-8") as fp:
                    count = sum(1 for line in fp if line.strip())
            except Exception as exc:
                self._last_archive_error = str(exc)
        return {
            "available": bool(self._archive_path),
            "path": str(self._archive_path or ""),
            "exists": exists,
            "count": count,
            "last_error": self._last_archive_error,
        }

    def list_sqlite_reports(self, limit: int = 100) -> List[Dict[str, Any]]:
        cap = max(1, int(limit or 100))
        rows: List[Dict[str, Any]] = []
        with self._lock:
            if not self._sqlite_path or not os.path.isfile(self._sqlite_path):
                return []
            conn = None
            try:
                conn = sqlite3.connect(self._sqlite_path)
                conn.row_factory = sqlite3.Row
                for row in conn.execute(
                    "SELECT id, payload_json, archived_at FROM encounters ORDER BY completed_at DESC, id DESC LIMIT ?",
                    (cap,),
                ):
                    try:
                        item = json.loads(row["payload_json"] or "{}")
                    except Exception:
                        item = {}
                    if not isinstance(item, dict):
                        item = {}
                    if not isinstance(item.get("entities"), list):
                        entities = []
                        for combatant in conn.execute(
                            "SELECT uid, name, profession, damage_total, heal_total, damage_taken, dps, hps, damage_pct, is_self, fight_point "
                            "FROM combatants WHERE encounter_row_id=? ORDER BY rank ASC, id ASC",
                            (int(row["id"] or 0),),
                        ):
                            entities.append({
                                "uid": _coerce_int(combatant["uid"]),
                                "name": str(combatant["name"] or ""),
                                "profession": str(combatant["profession"] or ""),
                                "damage_total": _coerce_int(combatant["damage_total"]),
                                "heal_total": _coerce_int(combatant["heal_total"]),
                                "damage_taken": _coerce_int(combatant["damage_taken"]),
                                "dps": _coerce_int(combatant["dps"]),
                                "hps": _coerce_int(combatant["hps"]),
                                "damage_pct": _coerce_float(combatant["damage_pct"]),
                                "is_self": bool(combatant["is_self"]),
                                "fight_point": _coerce_int(combatant["fight_point"]),
                            })
                        item["entities"] = entities
                    item.setdefault("_sqlite_schema_version", DPS_HISTORY_SQLITE_SCHEMA_VERSION)
                    item.setdefault("_sqlite_row_id", _coerce_int(row["id"]))
                    item.setdefault("_sqlite_archived_at", str(row["archived_at"] or ""))
                    rows.append(item)
                self._last_sqlite_error = ""
            except Exception as exc:
                self._last_sqlite_error = str(exc)
                return []
            finally:
                try:
                    if conn is not None:
                        conn.close()
                except Exception:
                    pass
        return rows

    def list_sqlite_actions(self, limit: int = 100, encounter_id: str = "") -> List[Dict[str, Any]]:
        cap = max(1, int(limit or 100))
        wanted_encounter = str(encounter_id or "").strip()
        rows: List[Dict[str, Any]] = []
        with self._lock:
            if not self._sqlite_path or not os.path.isfile(self._sqlite_path):
                return []
            conn = None
            try:
                conn = sqlite3.connect(self._sqlite_path)
                conn.row_factory = sqlite3.Row
                self._ensure_sqlite_schema_locked(conn)
                conn.commit()
                sql = (
                    "SELECT a.*, e.encounter_id FROM actions a "
                    "JOIN encounters e ON e.id = a.encounter_row_id "
                )
                params: tuple[Any, ...]
                if wanted_encounter:
                    sql += "WHERE e.encounter_id = ? "
                    params = (wanted_encounter, cap)
                else:
                    params = (cap,)
                sql += "ORDER BY e.completed_at DESC, a.time_ms ASC, a.seq ASC LIMIT ?"
                for row in conn.execute(sql, params):
                    try:
                        payload = json.loads(row["payload_json"] or "{}")
                    except Exception:
                        payload = {}
                    rows.append({
                        "encounter_id": str(row["encounter_id"] or ""),
                        "seq": _coerce_int(row["seq"]),
                        "topic": str(row["topic"] or ""),
                        "action_type": str(row["action_type"] or ""),
                        "observed_at": _coerce_float(row["observed_at"]),
                        "time_ms": _coerce_int(row["time_ms"]),
                        "actor_uid": _coerce_int(row["actor_uid"]),
                        "actor_name": str(row["actor_name"] or ""),
                        "target_uid": _coerce_int(row["target_uid"]),
                        "target_name": str(row["target_name"] or ""),
                        "skill_id": str(row["skill_id"] or ""),
                        "skill_name": str(row["skill_name"] or ""),
                        "value": _coerce_float(row["value"]),
                        "source_name": str(row["source_name"] or ""),
                        "source_kind": str(row["source_kind"] or ""),
                        "payload": payload,
                    })
                self._last_sqlite_error = ""
            except Exception as exc:
                self._last_sqlite_error = str(exc)
                return []
            finally:
                try:
                    if conn is not None:
                        conn.close()
                except Exception:
                    pass
        return rows

    def sqlite_status(self) -> Dict[str, Any]:
        encounter_count = 0
        combatant_count = 0
        action_count = 0
        timeline_count = 0
        trigger_count = 0
        source_metadata_count = 0
        schema_version = 0
        with self._lock:
            exists = bool(self._sqlite_path and os.path.isfile(self._sqlite_path))
            # 写代缓存命中 — 没有新写入时直接复用上次统计
            cache_key = (self._sqlite_write_gen, str(self._sqlite_path or ""), exists)
            if self._sqlite_status_cache is not None \
                    and self._sqlite_status_cache[0] == cache_key:
                return dict(self._sqlite_status_cache[1])
            if exists:
                conn = None
                try:
                    conn = sqlite3.connect(self._sqlite_path)
                    self._ensure_sqlite_schema_locked(conn)
                    conn.commit()
                    try:
                        schema_version = _coerce_int(conn.execute(
                            "SELECT value FROM meta WHERE key='schema_version'"
                        ).fetchone()[0])
                    except Exception:
                        schema_version = 0
                    encounter_count = _coerce_int(conn.execute("SELECT COUNT(*) FROM encounters").fetchone()[0])
                    combatant_count = _coerce_int(conn.execute("SELECT COUNT(*) FROM combatants").fetchone()[0])
                    action_count = _coerce_int(conn.execute("SELECT COUNT(*) FROM actions").fetchone()[0])
                    timeline_count = _coerce_int(conn.execute("SELECT COUNT(*) FROM timeline_events").fetchone()[0])
                    trigger_count = _coerce_int(conn.execute("SELECT COUNT(*) FROM trigger_events").fetchone()[0])
                    source_metadata_count = _coerce_int(conn.execute("SELECT COUNT(*) FROM source_metadata").fetchone()[0])
                    self._last_sqlite_error = ""
                except Exception as exc:
                    self._last_sqlite_error = str(exc)
                finally:
                    try:
                        if conn is not None:
                            conn.close()
                    except Exception:
                        pass
            status = {
                "available": bool(self._sqlite_path),
                "path": str(self._sqlite_path or ""),
                "exists": exists,
                "schema_version": schema_version or (DPS_HISTORY_SQLITE_SCHEMA_VERSION if exists and not self._last_sqlite_error else 0),
                "encounter_count": encounter_count,
                "combatant_count": combatant_count,
                "action_count": action_count,
                "timeline_count": timeline_count,
                "trigger_count": trigger_count,
                "source_metadata_count": source_metadata_count,
                "last_error": self._last_sqlite_error,
            }
            # 出错时不缓存 — 锁竞争/损坏是暂态, 下次轮询重试
            if not self._last_sqlite_error:
                self._sqlite_status_cache = (cache_key, dict(status))
            return status

    def export_report(self, report: Optional[Dict[str, Any]] = None,
                      fmt: str = "json") -> Optional[str]:
        src = report if isinstance(report, dict) else self.latest_report()
        if not isinstance(src, dict):
            return None
        item = _compact_report(src)
        fmt = str(fmt or "json").strip().lower()
        if fmt not in ("json", "csv", "html", "xml", "xml.gz", "xml.zip"):
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
        elif fmt == "xml":
            with open(path, "w", encoding="utf-8") as fp:
                fp.write(_render_xml_report(item))
        elif fmt == "xml.gz":
            with gzip.open(path, "wt", encoding="utf-8") as fp:
                fp.write(_render_xml_report(item))
        elif fmt == "xml.zip":
            xml_name = f"dps_{stamp}_{reason}.xml"
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr(xml_name, _render_xml_report(item))
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
    "DPS_HISTORY_JSONL_PATH",
    "DPS_HISTORY_JSONL_SCHEMA_VERSION",
    "DPS_HISTORY_PATH",
    "DPS_HISTORY_SQLITE_PATH",
    "DPS_HISTORY_SQLITE_SCHEMA_VERSION",
    "DpsHistoryStore",
    "load_exported_report_file",
    "load_exported_xml_report",
]
