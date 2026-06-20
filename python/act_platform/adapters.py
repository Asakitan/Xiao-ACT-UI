# -*- coding: utf-8 -*-
"""ACT parser adapter contracts and built-in parser wrappers."""

from __future__ import annotations

from abc import ABC, abstractmethod
import copy
import base64
import json
import os
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Any, Callable, Mapping, Optional

from .events import make_event


def _str_tuple(value: Any, default: tuple[str, ...] = ()) -> tuple[str, ...]:
    if value is None:
        return tuple(default)
    if isinstance(value, str):
        text = value.strip()
        return (text,) if text else tuple(default)
    if isinstance(value, (list, tuple, set)):
        out = tuple(str(item).strip() for item in value if str(item or "").strip())
        return out or tuple(default)
    return tuple(default)


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return float(default)


def _event_rows(value: Any) -> list[dict[str, Any]]:
    if isinstance(value, Mapping):
        events = value.get("events")
        if isinstance(events, (list, tuple)):
            return [dict(item) for item in events if isinstance(item, Mapping)]
        event = value.get("event")
        if isinstance(event, Mapping):
            return [dict(event)]
        if value.get("topic"):
            return [dict(value)]
        return []
    if isinstance(value, (list, tuple)):
        return [dict(item) for item in value if isinstance(item, Mapping)]
    return []


def _json_worker_safe(value: Any) -> Any:
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {"__act_bytes_b64__": base64.b64encode(bytes(value)).decode("ascii")}
    if isinstance(value, Mapping):
        return {str(key): _json_worker_safe(val) for key, val in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_json_worker_safe(item) for item in value]
    return value


@dataclass(frozen=True)
class ParserAdapterMetadata:
    """Serializable description of a parser/game adapter."""

    adapter_id: str
    game_id: str
    display_name: str
    supported_locales: tuple[str, ...] = field(default_factory=lambda: ("zh-CN",))
    source_kinds: tuple[str, ...] = field(default_factory=lambda: ("packet",))
    priority: float = 0.0
    version: str = "1"
    description: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": str(self.adapter_id or ""),
            "adapter_id": str(self.adapter_id or ""),
            "game_id": str(self.game_id or ""),
            "display_name": str(self.display_name or ""),
            "supported_locales": list(self.supported_locales),
            "source_kinds": list(self.source_kinds),
            "priority": float(self.priority or 0.0),
            "version": str(self.version or ""),
            "description": str(self.description or ""),
        }

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "ParserAdapterMetadata":
        adapter_id = str(value.get("adapter_id") or value.get("id") or "").strip()
        display_name = str(value.get("display_name") or value.get("title") or "").strip() or adapter_id
        return cls(
            adapter_id=adapter_id,
            game_id=str(value.get("game_id") or "").strip(),
            display_name=display_name,
            supported_locales=_str_tuple(value.get("supported_locales") or value.get("locales"), ("zh-CN",)),
            source_kinds=_str_tuple(value.get("source_kinds"), ("packet",)),
            priority=_safe_float(value.get("priority"), 0.0),
            version=str(value.get("version") or "1"),
            description=str(value.get("description") or ""),
        )


class ParserAdapter(ABC):
    """Small runtime contract for parser/game integrations."""

    def __init__(self, metadata: ParserAdapterMetadata) -> None:
        self.metadata = metadata
        self.started = False

    @property
    def adapter_id(self) -> str:
        return self.metadata.adapter_id

    @property
    def game_id(self) -> str:
        return self.metadata.game_id

    def start(self) -> "ParserAdapter":
        self.started = True
        return self

    def stop(self) -> None:
        self.started = False

    @abstractmethod
    def parse_packet(self, frame: bytes) -> Any:
        ...

    def parse_log_line(self, line: str) -> list[dict[str, Any]]:
        return []

    def import_file(self, path: str) -> dict[str, Any]:
        return {
            "ok": False,
            "adapter_id": self.adapter_id,
            "reason": "import_file_not_supported",
            "path": str(path or ""),
        }

    def normalize_event(
        self,
        topic: str,
        payload: Optional[Mapping[str, Any]] = None,
        *,
        source_kind: str = "packet",
        confidence: float = 1.0,
        observed_at: Optional[float] = None,
    ) -> dict[str, Any]:
        return make_event(
            topic,
            payload,
            source_name=self.adapter_id,
            source_kind=source_kind,
            game_id=self.game_id,
            parser_id=self.adapter_id,
            confidence=confidence,
            observed_at=observed_at,
        )

    def health(self) -> dict[str, Any]:
        return {
            "adapter_id": self.adapter_id,
            "game_id": self.game_id,
            "started": bool(self.started),
            "metadata": self.metadata.to_dict(),
        }




class PluginParserAdapter(ParserAdapter):
    """Controlled runtime wrapper for plugin-declared parser adapters."""

    def __init__(self, manager: Any, metadata: Mapping[str, Any] | ParserAdapterMetadata,
                 *, time_budget_ms: float = 25.0) -> None:
        raw = metadata if isinstance(metadata, Mapping) else {}
        meta = metadata if isinstance(metadata, ParserAdapterMetadata) else ParserAdapterMetadata.from_mapping(raw)
        super().__init__(meta)
        self.manager = manager
        self.plugin_id = str(raw.get("plugin_id") or "")
        self.isolation = str(raw.get("isolation") or raw.get("isolation_mode") or "in_process").strip().lower()
        if self.isolation in {"process", "external_process", "subprocess"}:
            self.isolation = "process"
        else:
            self.isolation = "in_process"
        budget = raw.get("time_budget_ms") if isinstance(raw, Mapping) else None
        if budget is None and isinstance(raw, Mapping):
            budget = raw.get("max_runtime_ms")
        default_budget = 1000.0 if self.isolation == "process" else time_budget_ms
        self.time_budget_ms = _safe_float(budget, default_budget)
        self._last_invocation: dict[str, Any] = {}

    def _plugin_path(self) -> str:
        records = getattr(self.manager, "_records", {})
        record = records.get(self.plugin_id) if isinstance(records, Mapping) else None
        return str(getattr(record, "path", "") or "")

    def _record_process_failure(self, message: str) -> None:
        record_failure = getattr(self.manager, "_record_failure", None)
        if callable(record_failure) and self.plugin_id:
            try:
                record_failure(self.plugin_id, RuntimeError(str(message or "process parser adapter failed")))
            except Exception:
                pass

    def _invoke_process(self, request: Mapping[str, Any]) -> dict[str, Any]:
        plugin_path = self._plugin_path()
        if not plugin_path or not os.path.isdir(plugin_path):
            message = "plugin path is unavailable for process-isolated parser adapter"
            self._record_process_failure(message)
            return {
                "ok": False,
                "plugin_id": self.plugin_id,
                "extension_id": self.adapter_id,
                "message": message,
                "errors": [message],
                "isolation": "process",
            }
        payload = {
            "plugin_path": plugin_path,
            "adapter_id": self.adapter_id,
            "payload": _json_worker_safe(dict(request)),
        }
        timeout_s = max(0.1, float(self.time_budget_ms or 25.0) / 1000.0)
        try:
            proc = subprocess.run(
                [sys.executable, "-m", "act_platform.parser_worker"],
                input=json.dumps(payload, ensure_ascii=False),
                text=True,
                encoding="utf-8",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout_s,
                check=False,
            )
        except subprocess.TimeoutExpired:
            message = f"process parser adapter exceeded budget: > {self.time_budget_ms:.1f}ms"
            self._record_process_failure(message)
            return {
                "ok": False,
                "plugin_id": self.plugin_id,
                "extension_id": self.adapter_id,
                "time_budget_ms": self.time_budget_ms,
                "timed_out": True,
                "message": message,
                "errors": [message],
                "isolation": "process",
            }
        except Exception as exc:
            message = str(exc)
            self._record_process_failure(message)
            return {
                "ok": False,
                "plugin_id": self.plugin_id,
                "extension_id": self.adapter_id,
                "message": message,
                "errors": [message],
                "isolation": "process",
            }
        try:
            result = json.loads(proc.stdout or "{}")
        except Exception:
            result = {}
        if proc.returncode != 0 and not result:
            message = (proc.stderr or "").strip() or f"worker exited with {proc.returncode}"
            result = {"ok": False, "message": message, "errors": [message]}
        if not isinstance(result, Mapping):
            result = {"ok": False, "message": "worker returned non-object result", "errors": ["worker returned non-object result"]}
        out = dict(result)
        out.setdefault("plugin_id", self.plugin_id)
        out.setdefault("extension_id", self.adapter_id)
        out.setdefault("time_budget_ms", self.time_budget_ms)
        out["isolation"] = "process"
        if not bool(out.get("ok")):
            self._record_process_failure(str(out.get("message") or "; ".join(out.get("errors") or []) or "process parser adapter failed"))
        return out

    def invoke(self, operation: str, payload: Optional[Mapping[str, Any]] = None) -> dict[str, Any]:
        request = {"operation": str(operation or "")}
        if isinstance(payload, Mapping):
            request.update(copy.deepcopy(dict(payload)))
        if self.isolation == "process":
            result = self._invoke_process(request)
        else:
            invoke_extension = getattr(self.manager, "invoke_extension", None)
            if not callable(invoke_extension):
                result = {
                    "ok": False,
                    "plugin_id": self.plugin_id,
                    "extension_id": self.adapter_id,
                    "message": "plugin manager cannot invoke extensions",
                    "errors": ["plugin manager cannot invoke extensions"],
                }
            else:
                result = invoke_extension(
                    "parser_adapters",
                    self.adapter_id,
                    request,
                    time_budget_ms=self.time_budget_ms,
                )
                if not isinstance(result, Mapping):
                    result = {
                        "ok": False,
                        "plugin_id": self.plugin_id,
                        "extension_id": self.adapter_id,
                        "message": "parser adapter handler returned a non-mapping invocation envelope",
                        "errors": ["invalid invocation envelope"],
                        "result": result,
                    }
        self._last_invocation = {
            "operation": request["operation"],
            "ok": bool(result.get("ok")),
            "elapsed_ms": result.get("elapsed_ms"),
            "time_budget_ms": result.get("time_budget_ms", self.time_budget_ms),
            "timed_out": bool(result.get("timed_out")),
            "isolation": self.isolation,
            "errors": list(result.get("errors") or []),
        }
        return dict(result)

    def start(self) -> "PluginParserAdapter":
        result = self.invoke("start")
        self.started = bool(result.get("ok"))
        return self

    def stop(self) -> None:
        if self.started:
            self.invoke("stop")
        super().stop()

    def parse_packet(self, frame: bytes) -> dict[str, Any]:
        frame_bytes = bytes(frame or b"")
        return self.invoke("parse_packet", {
            "frame": frame_bytes,
            "frame_len": len(frame_bytes),
        })

    def parse_log_line(self, line: str) -> list[dict[str, Any]]:
        result = self.invoke("parse_log_line", {"line": str(line or "")})
        return _event_rows(result.get("result")) if result.get("ok") else []

    def import_file(self, path: str) -> dict[str, Any]:
        return self.invoke("import_file", {"path": str(path or "")})

    def normalize_event(
        self,
        topic: str,
        payload: Optional[Mapping[str, Any]] = None,
        *,
        source_kind: str = "packet",
        confidence: float = 1.0,
        observed_at: Optional[float] = None,
    ) -> dict[str, Any]:
        result = self.invoke("normalize_event", {
            "topic": str(topic or ""),
            "payload": copy.deepcopy(dict(payload or {})),
            "source_kind": str(source_kind or "packet"),
            "confidence": float(confidence),
            "observed_at": observed_at,
        })
        normalized = result.get("result") if result.get("ok") else None
        if isinstance(normalized, Mapping):
            return dict(normalized)
        return super().normalize_event(
            topic,
            payload,
            source_kind=source_kind,
            confidence=confidence,
            observed_at=observed_at,
        )

    def health(self) -> dict[str, Any]:
        out = super().health()
        out.update({
            "plugin_id": self.plugin_id,
            "extension_id": self.adapter_id,
            "handler_kind": "parser_adapters",
            "isolation": self.isolation,
            "last_invocation": dict(self._last_invocation),
        })
        return out


def built_in_parser_adapters() -> list[dict[str, Any]]:
    return []


def create_builtin_parser_adapter(adapter_id: str = "") -> ParserAdapter:
    raise KeyError(f"unknown built-in parser adapter: {adapter_id}")


def plugin_parser_adapters(manager: Any) -> list[dict[str, Any]]:
    list_extensions = getattr(manager, "list_extensions", None)
    if not callable(list_extensions):
        return []
    rows = list_extensions("parser_adapters")
    if not isinstance(rows, list):
        return []
    out: list[dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, Mapping):
            continue
        item = ParserAdapterMetadata.from_mapping(row).to_dict()
        for key in ("plugin_id", "time_budget_ms", "max_runtime_ms", "isolation", "isolation_mode"):
            if key in row:
                item[key] = row[key]
        out.append(item)
    return out


def create_plugin_parser_adapter(manager: Any, adapter_id: str) -> PluginParserAdapter:
    target = str(adapter_id or "").strip()
    list_extensions = getattr(manager, "list_extensions", None)
    rows = list_extensions("parser_adapters") if callable(list_extensions) else []
    if isinstance(rows, list):
        for row in rows:
            if isinstance(row, Mapping) and str(row.get("id") or row.get("adapter_id") or "") == target:
                return PluginParserAdapter(manager, row)
    raise KeyError(f"unknown plugin parser adapter: {target}")


__all__ = [
    "ParserAdapter",
    "ParserAdapterMetadata",
    "PluginParserAdapter",
    "built_in_parser_adapters",
    "create_builtin_parser_adapter",
    "create_plugin_parser_adapter",
    "plugin_parser_adapters",
]
