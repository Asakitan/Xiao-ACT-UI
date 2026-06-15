# -*- coding: utf-8 -*-
"""One-shot process worker for plugin parser-adapter handlers."""

from __future__ import annotations

import base64
import contextlib
import importlib.util
import io
import json
import os
import sys
import time
from types import ModuleType
from typing import Any, Callable, Mapping


def _json_safe(value: Any) -> Any:
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, Mapping):
        return {str(key): _json_safe(val) for key, val in value.items() if not callable(val)}
    if isinstance(value, (list, tuple, set)):
        return [_json_safe(item) for item in value if not callable(item)]
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {"__act_bytes_b64__": base64.b64encode(bytes(value)).decode("ascii")}
    return str(value)


def _decode_worker_value(value: Any) -> Any:
    if isinstance(value, Mapping):
        if set(value.keys()) == {"__act_bytes_b64__"}:
            try:
                return base64.b64decode(str(value.get("__act_bytes_b64__") or ""))
            except Exception:
                return b""
        return {str(key): _decode_worker_value(val) for key, val in value.items()}
    if isinstance(value, list):
        return [_decode_worker_value(item) for item in value]
    return value


class _WorkerContext:
    def __init__(self) -> None:
        self.parser_handlers: dict[str, Callable[[dict[str, Any]], Any]] = {}
        self.parser_metadata: dict[str, dict[str, Any]] = {}
        self.logs: list[str] = []
        self.defaults: dict[str, Any] = {}

    def register_parser_adapter(self, adapter_id: str, metadata: Mapping[str, Any] | None = None,
                                handler: Callable[[dict[str, Any]], Any] | None = None) -> str:
        adapter_id = str(adapter_id or "").strip()
        if not adapter_id:
            raise ValueError("adapter_id is required")
        self.parser_metadata[adapter_id] = dict(metadata or {})
        if callable(handler):
            self.parser_handlers[adapter_id] = handler
        return adapter_id

    def log(self, message: Any) -> None:
        self.logs.append(str(message))

    def setting(self, key: str, default: Any = None) -> Any:
        return self.defaults.get(str(key or ""), default)

    def get_setting(self, key: str, default: Any = None) -> Any:
        return self.setting(key, default)

    def set_defaults(self, mapping: Mapping[str, Any]) -> None:
        self.defaults.update(dict(mapping or {}))


def _load_module(plugin_path: str, entry: str) -> ModuleType:
    entry_path = os.path.join(plugin_path, entry)
    if plugin_path not in sys.path:
        sys.path.insert(0, plugin_path)
    spec = importlib.util.spec_from_file_location(f"act_worker_plugin_{abs(hash(entry_path))}", entry_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load plugin entry: {entry}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _read_manifest(plugin_path: str) -> dict[str, Any]:
    manifest_path = os.path.join(plugin_path, "plugin.json")
    with open(manifest_path, "r", encoding="utf-8") as fp:
        data = json.load(fp)
    if not isinstance(data, Mapping):
        raise RuntimeError("plugin manifest must be an object")
    return dict(data)


def invoke_once(request: Mapping[str, Any]) -> dict[str, Any]:
    plugin_path = os.path.abspath(str(request.get("plugin_path") or ""))
    adapter_id = str(request.get("adapter_id") or "").strip()
    payload = _decode_worker_value(request.get("payload") or {})
    if not plugin_path or not os.path.isdir(plugin_path):
        raise RuntimeError("plugin_path is invalid")
    if not adapter_id:
        raise RuntimeError("adapter_id is required")
    manifest = _read_manifest(plugin_path)
    ctx = _WorkerContext()
    stdout_capture = io.StringIO()
    with contextlib.redirect_stdout(stdout_capture):
        module = _load_module(plugin_path, str(manifest.get("entry") or "plugin.py"))
        on_load = getattr(module, "on_load", None)
        if callable(on_load):
            on_load(ctx)
        handler = ctx.parser_handlers.get(adapter_id)
        if not callable(handler):
            raise RuntimeError(f"parser adapter handler is unavailable: {adapter_id}")
        started = time.perf_counter()
        result = handler(dict(payload if isinstance(payload, Mapping) else {}))
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    printed = stdout_capture.getvalue().strip()
    if printed:
        ctx.logs.append(printed)
    return {
        "ok": True,
        "plugin_id": str(manifest.get("id") or ""),
        "extension_id": adapter_id,
        "elapsed_ms": elapsed_ms,
        "result": _json_safe(result),
        "logs": list(ctx.logs[-20:]),
        "errors": [],
    }


def main() -> int:
    try:
        raw = sys.stdin.read()
        request = json.loads(raw or "{}")
        if not isinstance(request, Mapping):
            raise RuntimeError("worker request must be an object")
        result = invoke_once(request)
    except Exception as exc:
        result = {
            "ok": False,
            "message": str(exc),
            "errors": [str(exc)],
        }
    sys.stdout.write(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    sys.stdout.flush()
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
