# -*- coding: utf-8 -*-
"""Mini-Parse / clipboard formatter registry for ACT reports."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Callable, Mapping


Formatter = Callable[[Mapping[str, Any]], str]


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return int(default)


def _rows(payload: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    preview = payload.get("preview") if isinstance(payload.get("preview"), Mapping) else {}
    rows = preview.get("top_rows") if isinstance(preview, Mapping) else None
    if isinstance(rows, list) and rows:
        return [row for row in rows if isinstance(row, Mapping)]
    report = payload.get("report") if isinstance(payload.get("report"), Mapping) else {}
    entities = report.get("entities") if isinstance(report, Mapping) else None
    if isinstance(entities, list):
        return [row for row in entities if isinstance(row, Mapping)]
    return []


def _row_name(row: Mapping[str, Any]) -> str:
    return str(row.get("name") or row.get("actor") or row.get("uid") or row.get("id") or "-")


def _row_damage(row: Mapping[str, Any]) -> int:
    return _safe_int(row.get("damage") or row.get("damage_total") or row.get("total_damage"), 0)


def _row_dps(row: Mapping[str, Any]) -> int:
    return _safe_int(row.get("dps") or row.get("total_dps"), 0)


def _format_summary_table(payload: Mapping[str, Any]) -> str:
    preview = payload.get("preview") if isinstance(payload.get("preview"), Mapping) else {}
    title = str(preview.get("title") or "ACT Encounter") if isinstance(preview, Mapping) else "ACT Encounter"
    total_damage = _safe_int(preview.get("total_damage") if isinstance(preview, Mapping) else 0, 0)
    total_dps = _safe_int(preview.get("total_dps") if isinstance(preview, Mapping) else 0, 0)
    lines = [f"{title}  DMG {total_damage:,}  DPS {total_dps:,}"]
    for idx, row in enumerate(_rows(payload)[:8], 1):
        lines.append(f"#{idx:02d} {_row_name(row):<18} DMG {_row_damage(row):>10,} DPS {_row_dps(row):>8,}")
    return "\n".join(lines)


def _format_chat_ranking(payload: Mapping[str, Any]) -> str:
    parts = []
    for idx, row in enumerate(_rows(payload)[:8], 1):
        parts.append(f"#{idx} {_row_name(row)} {_row_damage(row):,}({_row_dps(row):,}/s)")
    return " | ".join(parts) if parts else "No ACT combatants"


def _format_json(payload: Mapping[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=False, indent=2, default=str)


@dataclass(frozen=True)
class MiniParseFormatter:
    formatter_id: str
    title: str
    description: str
    formatter: Formatter
    plugin_id: str = ""

    def status(self) -> dict[str, Any]:
        return {
            "id": self.formatter_id,
            "title": self.title,
            "description": self.description,
            "plugin_id": self.plugin_id,
        }


class MiniParseFormatterRegistry:
    """Registry for compact ACT clipboard/live text formatters."""

    def __init__(self) -> None:
        self._formatters: dict[str, MiniParseFormatter] = {}
        self.register("summary_table", "Summary table", "Multiline ACT mini-parse table", _format_summary_table)
        self.register("chat_ranking", "Chat ranking", "One-line chat-friendly ranking", _format_chat_ranking)
        self.register("json", "JSON payload", "Debug JSON payload", _format_json)

    def register(self, formatter_id: str, title: str, description: str, formatter: Formatter,
                 *, plugin_id: str = "") -> MiniParseFormatter:
        fmt_id = str(formatter_id or "").strip().lower().replace(" ", "_")
        if not fmt_id:
            raise ValueError("formatter id is required")
        if not callable(formatter):
            raise TypeError("formatter must be callable")
        row = MiniParseFormatter(
            formatter_id=fmt_id,
            title=str(title or fmt_id),
            description=str(description or ""),
            formatter=formatter,
            plugin_id=str(plugin_id or ""),
        )
        self._formatters[fmt_id] = row
        return row

    def list_formatters(self) -> list[dict[str, Any]]:
        return [self._formatters[key].status() for key in sorted(self._formatters)]

    def get(self, formatter_id: str | None = None) -> MiniParseFormatter:
        fmt_id = str(formatter_id or "summary_table").strip().lower().replace(" ", "_")
        return self._formatters.get(fmt_id) or self._formatters["summary_table"]

    def format(self, formatter_id: str | None, payload: Mapping[str, Any]) -> str:
        row = self.get(formatter_id)
        return str(row.formatter(payload))


def build_default_registry() -> MiniParseFormatterRegistry:
    return MiniParseFormatterRegistry()


__all__ = [
    "MiniParseFormatter",
    "MiniParseFormatterRegistry",
    "build_default_registry",
]
