# -*- coding: utf-8 -*-
# Offline import helpers for normalized ACT replay files.

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

from .fixture_io import load_events_jsonl


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _copy_event(item: Any, index: int) -> Dict[str, Any]:
    if not isinstance(item, dict):
        raise ValueError(f"event #{index} is not an object")
    return copy.deepcopy(item)


def _split_meta_events(events: Iterable[Dict[str, Any]], self_uid: int = 0) -> Tuple[int, List[Dict[str, Any]]]:
    out: List[Dict[str, Any]] = []
    uid = int(self_uid or 0)
    for index, event in enumerate(events, 1):
        item = _copy_event(event, index)
        if str(item.get("kind") or "").lower() == "meta":
            uid = _safe_int(item.get("self_uid"), uid)
            continue
        out.append(item)
    return uid, out


def _load_events_json(path: str | Path) -> Tuple[int, List[Dict[str, Any]]]:
    with Path(path).open("r", encoding="utf-8") as fp:
        data = json.load(fp)
    if isinstance(data, list):
        return _split_meta_events(data)
    if not isinstance(data, dict):
        raise ValueError("normalized replay JSON must be an object or event array")
    meta = data.get("metadata") if isinstance(data.get("metadata"), dict) else {}
    self_uid = _safe_int(data.get("self_uid") or meta.get("self_uid"), 0)
    events = (
        data.get("events")
        or data.get("act_replay_events")
        or data.get("items")
        or []
    )
    if not isinstance(events, list):
        raise ValueError("normalized replay JSON events must be an array")
    return _split_meta_events(events, self_uid)


def load_normalized_import(path: str | Path) -> Tuple[int, List[Dict[str, Any]]]:
    # Load normalized ACT replay events from JSONL/NDJSON or JSON.

    src = Path(path)
    suffix = src.suffix.lower()
    if suffix in (".jsonl", ".ndjson"):
        return load_events_jsonl(src)
    if suffix == ".json":
        return _load_events_json(src)
    raise ValueError(f"unsupported normalized replay import format: {suffix or src.name}")


def import_normalized_file(path: str | Path) -> dict[str, Any]:
    # Return a JSON-safe import summary and normalized event list.

    src = Path(path)
    try:
        self_uid, events = load_normalized_import(src)
        return {
            "ok": True,
            "format": src.suffix.lower().lstrip(".") or "unknown",
            "source_path": str(src),
            "self_uid": int(self_uid or 0),
            "event_count": len(events),
            "events": events,
            "errors": [],
        }
    except Exception as exc:
        return {
            "ok": False,
            "format": src.suffix.lower().lstrip(".") or "unknown",
            "source_path": str(src),
            "self_uid": 0,
            "event_count": 0,
            "events": [],
            "errors": [str(exc)],
            "message": str(exc),
        }


__all__ = ["import_normalized_file", "load_normalized_import"]
