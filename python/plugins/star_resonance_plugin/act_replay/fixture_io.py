# -*- coding: utf-8 -*-
# JSONL fixture helpers for offline ACT replay diagnostics.

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Tuple


FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures"


def fixture_path(name: str) -> Path:
    stem = str(name or "").strip()
    if not stem:
        raise ValueError("fixture name is empty")
    if not stem.endswith(".jsonl"):
        stem = f"{stem}.jsonl"
    path = (FIXTURE_DIR / stem).resolve()
    if FIXTURE_DIR.resolve() not in path.parents:
        raise ValueError(f"fixture path escapes fixture dir: {name!r}")
    return path


def load_events_jsonl(path: str | Path) -> Tuple[int, List[Dict[str, Any]]]:
    # Load normalized replay events from a JSONL fixture.
    #
    # The first non-empty line may be a metadata object with
    # ``{"kind":"meta","self_uid":...}``; all other lines are replay events.
    events: List[Dict[str, Any]] = []
    self_uid = 0
    with Path(path).open("r", encoding="utf-8") as fh:
        for line_no, line in enumerate(fh, 1):
            raw = line.strip()
            if not raw or raw.startswith("#"):
                continue
            item = json.loads(raw)
            if not isinstance(item, dict):
                raise ValueError(f"fixture line {line_no} is not an object")
            if str(item.get("kind") or "").lower() == "meta":
                self_uid = int(item.get("self_uid") or self_uid or 0)
                continue
            events.append(item)
    return self_uid, events


def load_fixture_events(name: str = "demo_events") -> Tuple[int, List[Dict[str, Any]]]:
    return load_events_jsonl(fixture_path(name))


__all__ = ["FIXTURE_DIR", "fixture_path", "load_events_jsonl", "load_fixture_events"]