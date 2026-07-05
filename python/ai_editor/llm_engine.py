"""Multi-provider LLM engine with streaming and tool-calling support.

Targets the OpenAI chat-completions wire format, which is also spoken by
Anthropic (via proxy), DeepSeek, Ollama, vLLM, and any compatible endpoint.
"""

from __future__ import annotations

import json
import threading
import time
import uuid
from urllib.parse import urljoin, urlsplit
from dataclasses import dataclass, field
from enum import Enum
from typing import (
    Any, Callable, Dict, Generator, List, Optional, Sequence, Tuple,
)


# ---------------------------------------------------------------------------
# Provider presets
# ---------------------------------------------------------------------------

class Provider(Enum):
    OPENAI = "openai"
    ANTHROPIC = "anthropic"
    DEEPSEEK = "deepseek"
    OLLAMA = "ollama"
    CUSTOM = "custom"


_PROVIDER_DEFAULTS: Dict[str, Dict[str, str]] = {
    Provider.OPENAI.value: {
        "base_url": "https://api.openai.com/v1",
        "default_model": "gpt-4o",
    },
    Provider.ANTHROPIC.value: {
        "base_url": "https://api.anthropic.com/v1",
        "default_model": "",
    },
    Provider.DEEPSEEK.value: {
        "base_url": "https://api.deepseek.com/v1",
        "default_model": "deepseek-chat",
    },
    Provider.OLLAMA.value: {
        "base_url": "http://localhost:11434/v1",
        "default_model": "llama3.1",
    },
    Provider.CUSTOM.value: {
        "base_url": "",
        "default_model": "",
    },
}

# Model registry — fully user-driven. No hardcoded models.
# Users add models via Settings → Custom Models or settings.json ai_editor.custom_models.
# Default context for unknown models: 128K input, 4K output, all capabilities enabled.
# Context window precedence: LLMConfig.max_input_tokens (global override)
# > user-registered entry (source="user", persisted) > provider-reported
# entry (source="api", in-memory only, never overrides user data) > default.

_DEFAULT_CONTEXT: Dict[str, int] = {"max_input": 128000, "max_output": 4096}
_DEFAULT_CAPS: Dict[str, bool] = {"tools": True, "vision": False, "thinking": False, "streaming": True}

COMPACTION_RATIO = 0.9

_model_registry: Dict[str, Dict[str, Any]] = {}


def register_model(name: str, max_input: int = 0, max_output: int = 0,
                    tools: bool = True, vision: bool = False,
                    thinking: bool = False, streaming: bool = True,
                    provider: str = "", base_url: str = "",
                    source: str = "user") -> None:
    """Register or update a model's context window and capabilities."""
    _model_registry[name] = {
        "max_input": max_input or _DEFAULT_CONTEXT["max_input"],
        "max_output": max_output or _DEFAULT_CONTEXT["max_output"],
        "tools": tools, "vision": vision,
        "thinking": thinking, "streaming": streaming,
        "source": str(source or "user"),
    }
    if provider:
        _model_registry[name]["provider"] = str(provider)
    if base_url:
        _model_registry[name]["base_url"] = str(base_url)


def register_api_context(name: str, max_input: int, max_output: int = 0) -> bool:
    """Record a provider-reported context window for a model.

    API-sourced values only fill models the user has not configured
    themselves; a user-registered entry always wins. Returns True when the
    registry was updated."""
    if not name or not max_input:
        return False
    existing = _model_registry.get(name)
    if existing and existing.get("source", "user") != "api":
        return False
    register_model(name, max_input=int(max_input), max_output=int(max_output or 0),
                   source="api")
    return True


def unregister_model(name: str) -> None:
    _model_registry.pop(name, None)


def set_custom_models(models: Dict[str, Any]) -> None:
    """Bulk load models from settings. Each value is a dict with optional keys:
    max_input, max_output, tools, vision, thinking, streaming."""
    for name, cfg in models.items():
        if isinstance(cfg, dict):
            register_model(
                name,
                max_input=cfg.get("max_input", 0),
                max_output=cfg.get("max_output", 0),
                tools=cfg.get("tools", True),
                vision=cfg.get("vision", False),
                thinking=cfg.get("thinking", False),
                streaming=cfg.get("streaming", True),
                provider=str(cfg.get("provider", "") or cfg.get("provider_id", "") or ""),
                base_url=str(cfg.get("base_url", "") or cfg.get("endpoint", "") or ""),
                source=str(cfg.get("source", "user") or "user"),
            )


def get_model_context(model: str) -> Dict[str, int]:
    """Return {max_input, max_output} for a model.
    Checks registry first, then prefix match, then default 128K."""
    entry = _model_registry.get(model)
    if entry:
        return {"max_input": entry["max_input"], "max_output": entry["max_output"]}
    for prefix, e in _model_registry.items():
        if model.startswith(prefix.rsplit("-", 1)[0]):
            return {"max_input": e["max_input"], "max_output": e["max_output"]}
    return dict(_DEFAULT_CONTEXT)


def get_model_capabilities(model: str) -> Dict[str, bool]:
    """Return per-model feature flags. Unknown models get all-enabled defaults."""
    entry = _model_registry.get(model)
    if entry:
        return {k: entry.get(k, v) for k, v in _DEFAULT_CAPS.items()}
    for prefix, e in _model_registry.items():
        if model.startswith(prefix.rsplit("-", 1)[0]):
            return {k: e.get(k, v) for k, v in _DEFAULT_CAPS.items()}
    return dict(_DEFAULT_CAPS)


def list_all_models() -> Dict[str, Dict[str, Any]]:
    """Return all registered models."""
    return dict(_model_registry)


def compaction_threshold(model: str) -> int:
    """Token count at which conversation should be compacted (90% of max input)."""
    ctx = get_model_context(model)
    return int(ctx["max_input"] * COMPACTION_RATIO)


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class ToolCall:
    id: str
    name: str
    arguments: str  # raw JSON string
    result: Optional[str] = None


@dataclass
class StreamDelta:
    """A single incremental chunk from the LLM stream."""
    content: str = ""
    thinking: str = ""
    tool_calls: List[Dict[str, Any]] = field(default_factory=list)
    data_parts: List[Dict[str, Any]] = field(default_factory=list)
    finish_reason: Optional[str] = None
    usage: Optional[Dict[str, int]] = None
    refusal: Optional[str] = None


@dataclass
class LLMResponse:
    content: str = ""
    thinking: str = ""
    tool_calls: List[ToolCall] = field(default_factory=list)
    finish_reason: str = ""
    usage: Dict[str, int] = field(default_factory=dict)
    error: Optional[str] = None
    model: str = ""
    refusal: Optional[str] = None


@dataclass
class ProviderConfig:
    provider: str = Provider.OPENAI.value
    api_key: str = ""
    base_url: str = ""
    model: str = ""
    temperature: float = 0.7
    max_tokens: int = 4096
    system_prompt: str = ""
    # Transport: chat_completions (default) or responses.
    transport: str = "chat_completions"
    # Advanced sampling
    top_p: float = 1.0
    frequency_penalty: float = 0.0
    presence_penalty: float = 0.0
    stop: List[str] = field(default_factory=list)
    # Context window overrides (0 = use MODEL_CONTEXT_WINDOWS default)
    max_input_tokens: int = 0
    max_output_tokens: int = 0
    # Connection
    timeout: int = 180
    extra_headers: Dict[str, str] = field(default_factory=dict)
    extra_body: Dict[str, Any] = field(default_factory=dict)

    @property
    def effective_base_url(self) -> str:
        if self.base_url:
            return self.base_url.rstrip("/")
        defaults = _PROVIDER_DEFAULTS.get(self.provider, {})
        return defaults.get("base_url", "").rstrip("/")

    @property
    def effective_model(self) -> str:
        if self.model:
            return self.model
        defaults = _PROVIDER_DEFAULTS.get(self.provider, {})
        return defaults.get("default_model", "")

    @property
    def effective_context(self) -> Dict[str, int]:
        ctx = get_model_context(self.effective_model)
        if self.max_input_tokens > 0:
            ctx["max_input"] = self.max_input_tokens
        if self.max_output_tokens > 0:
            ctx["max_output"] = self.max_output_tokens
        return ctx


def _friendly_llm_error(exc: Exception) -> str:
    """Turn a raw request exception into a short, user-facing Chinese message."""
    status = getattr(getattr(exc, "response", None), "status_code", None)
    if status in (401, 403):
        return "API Key 无效或无权限，请到设置中检查"
    if status == 404:
        return "接口地址不存在，请检查 Base URL"
    if status == 429:
        return "请求过于频繁（触发限流），请稍后重试"
    if isinstance(status, int) and status >= 500:
        return f"服务端错误 (HTTP {status})，请稍后重试"
    name = type(exc).__name__
    if "Connect" in name or "Timeout" in name:
        return f"无法连接服务商，请检查网络或 Base URL（{name}）"
    return f"{name}: {exc}"


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------

class LLMEngine:
    """Stateless LLM caller: given config + messages + tools → response / stream."""

    def __init__(self, config: Optional[ProviderConfig] = None):
        self.config = config or ProviderConfig()
        self._cancel = threading.Event()
        self._http: Any = None

    @property
    def _client(self):
        if self._http is None:
            import httpx
            t = self.config.timeout if self.config else 180
            self._http = httpx.Client(timeout=float(t), http2=False, follow_redirects=True)
        return self._http

    def close(self) -> None:
        if self._http is not None:
            try:
                self._http.close()
            except Exception:
                pass
            self._http = None

    def cancel(self) -> None:
        self._cancel.set()
        try:
            if self._http:
                self._http.close()
                self._http = None
        except Exception:
            pass

    def reset_cancel(self) -> None:
        self._cancel.clear()

    # -- Synchronous (blocking) call --

    def chat_completion(
        self,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]] = None,
        config_override: Optional[ProviderConfig] = None,
    ) -> LLMResponse:
        cfg = config_override or self.config
        try:
            headers = self._build_headers(cfg)
            if self._is_anthropic_native(cfg):
                body = self._build_body(cfg, messages, tools, stream=False)
                url = self._resolve_request_url(cfg, "/messages")
            elif self._uses_openai_responses(cfg):
                body = self._build_responses_body(cfg, messages, tools, stream=False)
                url = self._resolve_request_url(cfg, "/responses")
            else:
                body = self._build_body(cfg, messages, tools, stream=False)
                url = self._resolve_request_url(cfg, "/chat/completions")
            resp = self._client.post(url, json=body, headers=headers)
            resp.raise_for_status()
            data = resp.json()
            if self._is_anthropic_native(cfg):
                return self._parse_anthropic_response(data, cfg)
            if self._uses_openai_responses(cfg):
                return self._parse_responses_response(data, cfg)
            return self._parse_response(data, cfg)
        except Exception as exc:
            return LLMResponse(error=_friendly_llm_error(exc))

    # -- Streaming call --

    def chat_completion_stream(
        self,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]] = None,
        config_override: Optional[ProviderConfig] = None,
        on_delta: Optional[Callable[[StreamDelta], None]] = None,
    ) -> LLMResponse:
        """Blocking streaming call. Yields deltas via *on_delta* callback.

        Returns the fully-assembled LLMResponse when the stream ends.
        """
        cfg = config_override or self.config

        if self._is_anthropic_native(cfg):
            return self._stream_anthropic_native(cfg, messages, tools, on_delta)

        if self._uses_openai_responses(cfg):
            return self._stream_openai_responses(cfg, messages, tools, on_delta)

        return self._stream_openai(cfg, messages, tools, on_delta)

    # -- OpenAI-compatible SSE streaming --

    _RETRYABLE_STATUS = frozenset({429, 500, 502, 503})
    _MAX_RETRIES = 2
    _RETRY_BACKOFFS = (1.0, 2.0)

    def _stream_openai(
        self,
        cfg: ProviderConfig,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]],
        on_delta: Optional[Callable[[StreamDelta], None]],
    ) -> LLMResponse:
        body = self._build_body(cfg, messages, tools, stream=True)
        accumulated = LLMResponse(model=cfg.effective_model)
        tool_call_buffers: Dict[int, Dict[str, Any]] = {}
        _json_buffer: Dict[int, str] = {}  # partial JSON reassembly per tool call index

        headers = self._build_headers(cfg)
        url = self._resolve_request_url(cfg, "/chat/completions")
        last_exc: Optional[Exception] = None

        for _attempt in range(1 + self._MAX_RETRIES):
            if _attempt > 0:
                time.sleep(self._RETRY_BACKOFFS[min(_attempt - 1, len(self._RETRY_BACKOFFS) - 1)])
                accumulated = LLMResponse(model=cfg.effective_model)
                tool_call_buffers.clear()
                _json_buffer.clear()
            last_exc = None
            try:
                import httpx as _httpx
                with self._client.stream("POST", url, json=body, headers=headers) as resp:
                    if resp.status_code in self._RETRYABLE_STATUS and _attempt < self._MAX_RETRIES:
                        last_exc = Exception(f"HTTP {resp.status_code}")
                        continue
                    resp.raise_for_status()
                    for line in resp.iter_lines():
                        if self._cancel.is_set():
                            accumulated.finish_reason = "cancelled"
                            break
                        if not line or not line.startswith("data:"):
                            continue
                        payload = line[5:].strip()
                        if payload == "[DONE]":
                            break
                        try:
                            chunk = json.loads(payload)
                        except json.JSONDecodeError:
                            continue

                        delta = self._parse_stream_chunk(chunk)
                        if delta.content:
                            accumulated.content += delta.content
                        if delta.thinking:
                            accumulated.thinking += delta.thinking
                        if delta.finish_reason:
                            accumulated.finish_reason = delta.finish_reason
                        if delta.usage:
                            accumulated.usage = self._normalize_usage(delta.usage)
                        if delta.refusal:
                            accumulated.refusal = (accumulated.refusal or "") + delta.refusal

                        for tc_delta in delta.tool_calls:
                            idx = tc_delta.get("index", 0)
                            if idx not in tool_call_buffers:
                                tool_call_buffers[idx] = {
                                    "id": tc_delta.get("id", f"call_{uuid.uuid4().hex[:8]}"),
                                    "name": "",
                                    "arguments": "",
                                }
                                _json_buffer[idx] = ""
                            buf = tool_call_buffers[idx]
                            fn = tc_delta.get("function", {})
                            if fn.get("name"):
                                buf["name"] = fn["name"]
                            if fn.get("arguments"):
                                _json_buffer[idx] += fn["arguments"]
                                buf["arguments"] = _json_buffer[idx]

                        if on_delta:
                            on_delta(delta)
                # Stream completed successfully
                break

            except Exception as exc:
                last_exc = exc
                status = getattr(getattr(exc, 'response', None), 'status_code', None)
                if status in self._RETRYABLE_STATUS and _attempt < self._MAX_RETRIES:
                    continue
                accumulated.error = _friendly_llm_error(exc)
                break

        if last_exc and not accumulated.error:
            accumulated.error = _friendly_llm_error(last_exc)

        for _idx in sorted(tool_call_buffers):
            buf = tool_call_buffers[_idx]
            accumulated.tool_calls.append(
                ToolCall(id=buf["id"], name=buf["name"], arguments=buf["arguments"])
            )

        return accumulated

    # -- OpenAI Responses API SSE streaming --

    def _stream_openai_responses(
        self,
        cfg: ProviderConfig,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]],
        on_delta: Optional[Callable[[StreamDelta], None]],
    ) -> LLMResponse:
        body = self._build_responses_body(cfg, messages, tools, stream=True)
        accumulated = LLMResponse(model=cfg.effective_model)
        tool_call_buffers: Dict[Any, Dict[str, Any]] = {}
        tool_item_keys: Dict[str, Any] = {}
        completed: Optional[LLMResponse] = None

        headers = self._build_headers(cfg)
        url = self._resolve_request_url(cfg, "/responses")
        last_exc: Optional[Exception] = None

        for _attempt in range(1 + self._MAX_RETRIES):
            if _attempt > 0:
                time.sleep(self._RETRY_BACKOFFS[min(_attempt - 1, len(self._RETRY_BACKOFFS) - 1)])
                accumulated = LLMResponse(model=cfg.effective_model)
                tool_call_buffers.clear()
                tool_item_keys.clear()
                completed = None
            last_exc = None
            try:
                with self._client.stream("POST", url, json=body, headers=headers) as resp:
                    if resp.status_code in self._RETRYABLE_STATUS and _attempt < self._MAX_RETRIES:
                        last_exc = Exception(f"HTTP {resp.status_code}")
                        continue
                    resp.raise_for_status()
                    event_type = ""

                    def _remember_tool_key(key: Any, item: Dict[str, Any], data_obj: Dict[str, Any]) -> None:
                        for value in (
                            item.get("id"), item.get("call_id"),
                            data_obj.get("item_id"), data_obj.get("call_id"),
                        ):
                            if value:
                                tool_item_keys[str(value)] = key

                    def _tool_key(data_obj: Dict[str, Any], item: Optional[Dict[str, Any]] = None) -> Any:
                        for value in (data_obj.get("item_id"), data_obj.get("call_id")):
                            if value and str(value) in tool_item_keys:
                                return tool_item_keys[str(value)]
                        if data_obj.get("output_index") is not None:
                            return data_obj.get("output_index")
                        if item:
                            for value in (item.get("id"), item.get("call_id")):
                                if value and str(value) in tool_item_keys:
                                    return tool_item_keys[str(value)]
                            for value in (item.get("id"), item.get("call_id")):
                                if value:
                                    return value
                        return data_obj.get("item_id") or data_obj.get("call_id") or len(tool_call_buffers)

                    for line in resp.iter_lines():
                        if self._cancel.is_set():
                            accumulated.finish_reason = "cancelled"
                            break
                        if not line:
                            event_type = ""
                            continue
                        if line.startswith("event:"):
                            event_type = line[6:].strip()
                            continue
                        if not line.startswith("data:"):
                            continue
                        payload = line[5:].strip()
                        if payload == "[DONE]":
                            break
                        try:
                            data = json.loads(payload)
                        except json.JSONDecodeError:
                            continue

                        dtype = data.get("type", event_type)
                        if dtype == "response.output_text.delta":
                            text = data.get("delta", "")
                            if text:
                                accumulated.content += text
                                if on_delta:
                                    on_delta(StreamDelta(content=text))
                        elif dtype in (
                            "response.reasoning_text.delta",
                            "response.reasoning_summary_text.delta",
                        ):
                            text = data.get("delta", "")
                            if text:
                                accumulated.thinking += text
                                if on_delta:
                                    on_delta(StreamDelta(thinking=text))
                        elif dtype == "response.refusal.delta":
                            text = data.get("delta", "")
                            if text:
                                accumulated.refusal = (accumulated.refusal or "") + text
                        elif dtype == "response.output_item.added":
                            item = data.get("item", {})
                            if item.get("type") == "function_call":
                                key = _tool_key(data, item)
                                _remember_tool_key(key, item, data)
                                tool_call_buffers[key] = {
                                    "id": item.get("call_id") or item.get("id") or f"call_{uuid.uuid4().hex[:8]}",
                                    "name": item.get("name", ""),
                                    "arguments": self._coerce_tool_arguments(item.get("arguments", "")),
                                }
                        elif dtype == "response.function_call_arguments.delta":
                            key = _tool_key(data)
                            _remember_tool_key(key, {}, data)
                            buf = tool_call_buffers.setdefault(key, {
                                "id": data.get("call_id") or data.get("item_id") or f"call_{uuid.uuid4().hex[:8]}",
                                "name": data.get("name", ""),
                                "arguments": "",
                            })
                            if data.get("call_id"):
                                buf["id"] = data.get("call_id")
                            if data.get("name"):
                                buf["name"] = data.get("name")
                            buf["arguments"] += self._coerce_tool_arguments(data.get("delta", ""))
                        elif dtype == "response.function_call_arguments.done":
                            key = _tool_key(data)
                            _remember_tool_key(key, {}, data)
                            buf = tool_call_buffers.setdefault(key, {
                                "id": data.get("call_id") or data.get("item_id") or f"call_{uuid.uuid4().hex[:8]}",
                                "name": data.get("name", ""),
                                "arguments": "",
                            })
                            if data.get("call_id"):
                                buf["id"] = data.get("call_id")
                            if data.get("name"):
                                buf["name"] = data.get("name")
                            if data.get("arguments") is not None:
                                buf["arguments"] = self._coerce_tool_arguments(data.get("arguments", ""))
                        elif dtype == "response.output_item.done":
                            item = data.get("item", {})
                            if item.get("type") == "function_call":
                                key = _tool_key(data, item)
                                _remember_tool_key(key, item, data)
                                buf = tool_call_buffers.setdefault(key, {
                                    "id": item.get("call_id") or item.get("id") or f"call_{uuid.uuid4().hex[:8]}",
                                    "name": "",
                                    "arguments": "",
                                })
                                buf["id"] = item.get("call_id") or item.get("id") or buf["id"]
                                buf["name"] = item.get("name", buf["name"])
                                buf["arguments"] = self._coerce_tool_arguments(
                                    item.get("arguments", buf["arguments"])
                                )
                        elif dtype == "response.completed":
                            completed = self._parse_responses_response(data.get("response", {}), cfg)
                            break
                        elif dtype in ("response.failed", "response.incomplete"):
                            response = data.get("response", {})
                            err = response.get("error") or response.get("incomplete_details") or data
                            accumulated.error = err.get("message", str(err)) if isinstance(err, dict) else str(err)
                            break
                        elif dtype == "error":
                            err = data.get("error", data)
                            accumulated.error = err.get("message", str(err)) if isinstance(err, dict) else str(err)
                            break
                break

            except Exception as exc:
                last_exc = exc
                status = getattr(getattr(exc, 'response', None), 'status_code', None)
                if status in self._RETRYABLE_STATUS and _attempt < self._MAX_RETRIES:
                    continue
                accumulated.error = _friendly_llm_error(exc)
                break

        if last_exc and not accumulated.error:
            accumulated.error = _friendly_llm_error(last_exc)

        if completed:
            if not accumulated.content:
                accumulated.content = completed.content
            if completed.thinking and not accumulated.thinking:
                accumulated.thinking = completed.thinking
            if completed.refusal and not accumulated.refusal:
                accumulated.refusal = completed.refusal
            accumulated.finish_reason = completed.finish_reason
            accumulated.usage = self._normalize_usage(completed.usage)
            if tool_call_buffers:
                completed_by_id = {tc.id: tc for tc in completed.tool_calls}
                for buf in tool_call_buffers.values():
                    matching = completed_by_id.get(buf["id"])
                    if matching:
                        if not buf["name"]:
                            buf["name"] = matching.name
                        if not buf["arguments"]:
                            buf["arguments"] = matching.arguments
            else:
                accumulated.tool_calls = completed.tool_calls

        if tool_call_buffers:
            tool_calls = [
                ToolCall(id=buf["id"], name=buf["name"], arguments=buf["arguments"])
                for buf in tool_call_buffers.values()
            ]
            if completed:
                seen = {tc.id for tc in tool_calls}
                tool_calls.extend(tc for tc in completed.tool_calls if tc.id not in seen)
            accumulated.tool_calls = tool_calls

        return accumulated

    # -- Anthropic native SSE streaming --
    # Events: message_start, content_block_start, content_block_delta,
    #         content_block_stop, message_delta, message_stop

    def _stream_anthropic_native(
        self,
        cfg: ProviderConfig,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]],
        on_delta: Optional[Callable[[StreamDelta], None]],
    ) -> LLMResponse:
        body = self._build_body(cfg, messages, tools, stream=True)
        accumulated = LLMResponse(model=cfg.effective_model)
        tool_blocks: Dict[int, Dict[str, Any]] = {}  # index → {id, name, input_json}
        last_exc: Optional[Exception] = None

        headers = self._build_headers(cfg)
        url = self._resolve_request_url(cfg, "/messages")

        for _attempt in range(1 + self._MAX_RETRIES):
            if _attempt > 0:
                time.sleep(self._RETRY_BACKOFFS[min(_attempt - 1, len(self._RETRY_BACKOFFS) - 1)])
                accumulated = LLMResponse(model=cfg.effective_model)
                tool_blocks.clear()
            last_exc = None
            try:
                with self._client.stream("POST", url, json=body, headers=headers) as resp:
                    if resp.status_code in self._RETRYABLE_STATUS and _attempt < self._MAX_RETRIES:
                        last_exc = Exception(f"HTTP {resp.status_code}")
                        continue
                    resp.raise_for_status()
                    event_type = ""
                    for line in resp.iter_lines():
                        if self._cancel.is_set():
                            accumulated.finish_reason = "cancelled"
                            break
                        if not line:
                            event_type = ""
                            continue
                        if line.startswith("event:"):
                            event_type = line[6:].strip()
                            continue
                        if not line.startswith("data:"):
                            continue
                        payload = line[5:].strip()
                        try:
                            data = json.loads(payload)
                        except json.JSONDecodeError:
                            continue

                        dtype = data.get("type", "")

                        if dtype == "message_start":
                            msg = data.get("message", {})
                            accumulated.model = msg.get("model", cfg.effective_model)
                            usage = msg.get("usage", {})
                            if usage:
                                accumulated.usage = self._normalize_usage({
                                    **accumulated.usage,
                                    "input_tokens": usage.get("input_tokens", 0),
                                })

                        elif dtype == "content_block_start":
                            idx = data.get("index", 0)
                            block = data.get("content_block", {})
                            btype = block.get("type", "")
                            if btype == "tool_use":
                                tool_blocks[idx] = {
                                    "id": block.get("id", f"toolu_{uuid.uuid4().hex[:8]}"),
                                    "name": block.get("name", ""),
                                    "input_json": "",
                                }
                            elif btype == "thinking":
                                tool_blocks[idx] = {"_thinking": True}

                        elif dtype == "content_block_delta":
                            idx = data.get("index", 0)
                            d = data.get("delta", {})
                            delta_type = d.get("type", "")
                            if delta_type == "text_delta":
                                text = d.get("text", "")
                                if text:
                                    accumulated.content += text
                                    if on_delta:
                                        on_delta(StreamDelta(content=text))
                            elif delta_type == "thinking_delta":
                                text = d.get("thinking", "")
                                if text:
                                    accumulated.thinking += text
                                    if on_delta:
                                        on_delta(StreamDelta(thinking=text))
                            elif delta_type == "input_json_delta":
                                partial = d.get("partial_json", "")
                                if idx in tool_blocks and not tool_blocks[idx].get("_thinking"):
                                    tool_blocks[idx]["input_json"] += partial

                        elif dtype == "message_delta":
                            d = data.get("delta", {})
                            accumulated.finish_reason = d.get("stop_reason", "")
                            usage = data.get("usage", {})
                            if usage:
                                accumulated.usage = self._normalize_usage({
                                    **accumulated.usage,
                                    "output_tokens": usage.get("output_tokens", 0),
                                })

                        elif dtype == "message_stop":
                            break

                        elif dtype == "error":
                            err = data.get("error", {})
                            accumulated.error = err.get("message", str(data))
                            break
                # Stream completed successfully
                break

            except Exception as exc:
                last_exc = exc
                status = getattr(getattr(exc, 'response', None), 'status_code', None)
                if status in self._RETRYABLE_STATUS and _attempt < self._MAX_RETRIES:
                    continue
                accumulated.error = _friendly_llm_error(exc)
                break

        if last_exc and not accumulated.error:
            accumulated.error = _friendly_llm_error(last_exc)

        for _idx in sorted(tool_blocks):
            tb = tool_blocks[_idx]
            if not tb.get("_thinking"):
                accumulated.tool_calls.append(
                    ToolCall(id=tb["id"], name=tb["name"], arguments=tb["input_json"])
                )

        return accumulated

    # -- Anthropic native format --

    def _is_anthropic_native(self, cfg: ProviderConfig) -> bool:
        return (
            cfg.provider == Provider.ANTHROPIC.value
            and "anthropic.com" in cfg.effective_base_url
        )

    @staticmethod
    def _uses_openai_responses(cfg: ProviderConfig) -> bool:
        transport = (cfg.transport or "chat_completions").strip().lower().replace("-", "_")
        return transport in {"responses", "openai_responses", "response"}

    @classmethod
    def _resolve_request_url(cls, cfg: ProviderConfig, endpoint: str) -> str:
        endpoint_path = "/" + str(endpoint or "").lstrip("/")
        base = cls._normalize_request_base_url(cfg)
        split = urlsplit(base)
        current_path = split.path.rstrip("/")
        if current_path.endswith(endpoint_path.rstrip("/")):
            return base
        return urljoin(base.rstrip("/") + "/", endpoint_path.lstrip("/"))

    @staticmethod
    def _normalize_request_base_url(cfg: ProviderConfig) -> str:
        base = str(cfg.effective_base_url or "").strip()
        if not base:
            base = str(_PROVIDER_DEFAULTS.get(cfg.provider, {}).get("base_url", "") or "").strip()
        if not base:
            raise ValueError(
                f"No base URL configured for provider '{cfg.provider}'."
            )

        if base.startswith("//"):
            base = "https:" + base
        elif "://" not in base and not base.startswith("/"):
            base = "https://" + base.lstrip("/")

        split = urlsplit(base)
        if (not split.scheme or not split.netloc) and split.path.startswith("/"):
            default_base = str(_PROVIDER_DEFAULTS.get(cfg.provider, {}).get("base_url", "") or "").strip()
            default_split = urlsplit(default_base)
            if default_split.scheme and default_split.netloc:
                root = f"{default_split.scheme}://{default_split.netloc}/"
                base = urljoin(root, split.path.lstrip("/"))
                split = urlsplit(base)

        if not split.scheme or not split.netloc:
            raise ValueError(
                f"Invalid base URL for provider '{cfg.provider}': {base!r}"
            )
        return base.rstrip("/")

    # -- Internal helpers --

    def _build_headers(self, cfg: ProviderConfig) -> Dict[str, str]:
        headers: Dict[str, str] = {"Content-Type": "application/json"}
        if self._is_anthropic_native(cfg):
            headers["x-api-key"] = cfg.api_key
            headers["anthropic-version"] = "2023-06-01"
        elif cfg.api_key:
            headers["Authorization"] = f"Bearer {cfg.api_key}"
        headers.update(cfg.extra_headers)
        return headers

    def _build_body(
        self,
        cfg: ProviderConfig,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]],
        stream: bool,
    ) -> Dict[str, Any]:
        if self._is_anthropic_native(cfg):
            return self._build_anthropic_body(cfg, messages, tools, stream)
        body: Dict[str, Any] = {
            "model": cfg.effective_model,
            "messages": messages,
            "stream": stream,
            "temperature": cfg.temperature,
        }
        if cfg.top_p != 1.0:
            body["top_p"] = cfg.top_p
        if cfg.frequency_penalty != 0.0:
            body["frequency_penalty"] = cfg.frequency_penalty
        if cfg.presence_penalty != 0.0:
            body["presence_penalty"] = cfg.presence_penalty
        if cfg.stop:
            body["stop"] = cfg.stop
        if cfg.max_tokens > 0:
            body["max_tokens"] = cfg.max_tokens
        if tools:
            body["tools"] = tools
        if stream:
            body["stream_options"] = {"include_usage": True}
        if cfg.extra_body:
            body.update(cfg.extra_body)
        return body

    def _build_responses_body(
        self,
        cfg: ProviderConfig,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]],
        stream: bool,
    ) -> Dict[str, Any]:
        instructions, input_items = self._convert_messages_to_responses_input(messages)
        body: Dict[str, Any] = {
            "model": cfg.effective_model,
            "input": input_items,
            "stream": stream,
        }
        if instructions:
            body["instructions"] = instructions
        if cfg.temperature is not None:
            body["temperature"] = cfg.temperature
        if cfg.top_p != 1.0:
            body["top_p"] = cfg.top_p
        if cfg.max_tokens > 0:
            body["max_output_tokens"] = cfg.max_tokens
        if tools:
            body["tools"] = [self._convert_tool_to_responses(t) for t in tools]
        if cfg.extra_body:
            body.update(cfg.extra_body)
        return body

    def _build_anthropic_body(
        self,
        cfg: ProviderConfig,
        messages: List[Dict[str, Any]],
        tools: Optional[List[Dict[str, Any]]],
        stream: bool,
    ) -> Dict[str, Any]:
        system_parts = []
        conv: List[Dict[str, Any]] = []
        for msg in messages:
            if msg["role"] == "system":
                system_parts.append(msg["content"])
            else:
                conv.append(self._convert_msg_to_anthropic(msg))

        body: Dict[str, Any] = {
            "model": cfg.effective_model,
            "messages": conv,
            "stream": stream,
            "max_tokens": cfg.max_tokens or 4096,
        }
        if system_parts:
            body["system"] = "\n\n".join(system_parts)
        if cfg.temperature is not None:
            body["temperature"] = cfg.temperature
        if cfg.top_p != 1.0:
            body["top_p"] = cfg.top_p
        if cfg.stop:
            body["stop_sequences"] = cfg.stop
        if tools:
            body["tools"] = [self._convert_tool_to_anthropic(t) for t in tools]
        if cfg.extra_body:
            body.update(cfg.extra_body)
        return body

    @staticmethod
    def _convert_msg_to_anthropic(msg: Dict[str, Any]) -> Dict[str, Any]:
        role = msg["role"]
        if role == "tool":
            return {
                "role": "user",
                "content": [{
                    "type": "tool_result",
                    "tool_use_id": msg.get("tool_call_id", ""),
                    "content": msg.get("content", ""),
                }],
            }
        if role == "assistant" and msg.get("tool_calls"):
            blocks: list = []
            if msg.get("content"):
                blocks.append({"type": "text", "text": msg["content"]})
            for tc in msg["tool_calls"]:
                fn = tc.get("function", {})
                inp = LLMEngine._tool_input_from_arguments(fn.get("arguments", ""))
                blocks.append({
                    "type": "tool_use",
                    "id": tc.get("id", ""),
                    "name": fn.get("name", ""),
                    "input": inp,
                })
            return {"role": "assistant", "content": blocks}
        return {"role": role, "content": msg.get("content", "")}

    @staticmethod
    def _convert_tool_to_anthropic(tool: Dict[str, Any]) -> Dict[str, Any]:
        fn = tool.get("function", {})
        return {
            "name": fn.get("name", ""),
            "description": fn.get("description", ""),
            "input_schema": fn.get("parameters", {"type": "object", "properties": {}}),
        }

    @classmethod
    def _convert_messages_to_responses_input(
        cls,
        messages: List[Dict[str, Any]],
    ) -> Tuple[str, List[Dict[str, Any]]]:
        instructions: List[str] = []
        input_items: List[Dict[str, Any]] = []
        for msg in messages:
            role = msg.get("role", "user")
            content = msg.get("content", "")
            if role in {"system", "developer"}:
                text = cls._stringify_message_content(content)
                if text:
                    instructions.append(text)
                continue
            if role == "tool":
                input_items.append({
                    "type": "function_call_output",
                    "call_id": msg.get("tool_call_id", ""),
                    "output": cls._stringify_message_content(content),
                })
                continue
            if role == "assistant" and msg.get("tool_calls"):
                text = cls._stringify_message_content(content)
                if text:
                    input_items.append({"role": "assistant", "content": text})
                for tc in msg.get("tool_calls", []):
                    fn = tc.get("function", {})
                    input_items.append({
                        "type": "function_call",
                        "call_id": tc.get("id", ""),
                        "name": fn.get("name", ""),
                        "arguments": cls._coerce_tool_arguments(fn.get("arguments", "")),
                    })
                continue
            item_role = role if role in {"user", "assistant"} else "user"
            input_items.append({
                "role": item_role,
                "content": cls._convert_responses_message_content(content),
            })
        return "\n\n".join(instructions), input_items

    @staticmethod
    def _convert_tool_to_responses(tool: Dict[str, Any]) -> Dict[str, Any]:
        fn = tool.get("function", {})
        return {
            "type": "function",
            "name": fn.get("name", ""),
            "description": fn.get("description", ""),
            "parameters": fn.get("parameters", {"type": "object", "properties": {}}),
        }

    @classmethod
    def _convert_responses_message_content(cls, content: Any) -> Any:
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts: List[Dict[str, Any]] = []
            for part in content:
                if not isinstance(part, dict):
                    text = str(part)
                    if text:
                        parts.append({"type": "input_text", "text": text})
                    continue
                ptype = part.get("type")
                if ptype == "text":
                    text = part.get("text", "")
                    if text:
                        parts.append({"type": "input_text", "text": text})
                elif ptype == "image_url":
                    image = part.get("image_url", {})
                    url = image.get("url") if isinstance(image, dict) else image
                    if url:
                        parts.append({"type": "input_image", "image_url": url})
            return parts if parts else ""
        return cls._stringify_message_content(content)

    @staticmethod
    def _stringify_message_content(content: Any) -> str:
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            texts: List[str] = []
            for part in content:
                if isinstance(part, dict):
                    if part.get("type") == "text":
                        texts.append(str(part.get("text", "")))
                    elif "text" in part:
                        texts.append(str(part.get("text", "")))
                elif part is not None:
                    texts.append(str(part))
            return "\n".join(t for t in texts if t)
        if content is None:
            return ""
        return str(content)

    @staticmethod
    def _coerce_tool_arguments(value: Any) -> str:
        if value is None:
            return ""
        if isinstance(value, str):
            return value
        try:
            return json.dumps(value, ensure_ascii=False, default=str)
        except (TypeError, ValueError):
            return str(value)

    @staticmethod
    def _tool_input_from_arguments(value: Any) -> Dict[str, Any]:
        raw = LLMEngine._coerce_tool_arguments(value)
        if not raw.strip():
            return {}
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as exc:
            return {
                "_raw_arguments": raw,
                "_argument_parse_error": f"{exc.msg} at char {exc.pos}",
            }
        if isinstance(parsed, dict):
            return parsed
        return {"value": parsed}

    @staticmethod
    def _normalize_usage(usage: Any) -> Dict[str, Any]:
        if not isinstance(usage, dict):
            return {}
        result = dict(usage)

        def _as_int(value: Any) -> Optional[int]:
            if value is None or isinstance(value, bool):
                return None
            try:
                return int(value)
            except (TypeError, ValueError):
                return None

        input_tokens = _as_int(result.get("input_tokens"))
        if input_tokens is None:
            input_tokens = _as_int(result.get("prompt_tokens"))
        output_tokens = _as_int(result.get("output_tokens"))
        if output_tokens is None:
            output_tokens = _as_int(result.get("completion_tokens"))
        total_tokens = _as_int(result.get("total_tokens"))

        if input_tokens is not None:
            result["input_tokens"] = input_tokens
            result.setdefault("prompt_tokens", input_tokens)
        if output_tokens is not None:
            result["output_tokens"] = output_tokens
            result.setdefault("completion_tokens", output_tokens)
        if total_tokens is None and input_tokens is not None and output_tokens is not None:
            total_tokens = input_tokens + output_tokens
        if total_tokens is not None:
            result["total_tokens"] = total_tokens
        return result

    # -- Response parsing --

    @staticmethod
    def _parse_anthropic_response(data: Dict[str, Any], cfg: ProviderConfig) -> LLMResponse:
        """Parse Anthropic Messages API non-streaming response."""
        resp = LLMResponse(model=data.get("model", cfg.effective_model))
        try:
            resp.finish_reason = data.get("stop_reason", "")
            usage = data.get("usage", {})
            if usage:
                resp.usage = LLMEngine._normalize_usage(usage)
            for block in data.get("content", []):
                btype = block.get("type", "")
                if btype == "text":
                    resp.content += block.get("text", "")
                elif btype == "thinking":
                    resp.thinking += block.get("thinking", "")
                elif btype == "tool_use":
                    inp = block.get("input", {})
                    resp.tool_calls.append(ToolCall(
                        id=block.get("id", ""),
                        name=block.get("name", ""),
                        arguments=LLMEngine._coerce_tool_arguments(inp),
                    ))
        except (KeyError, TypeError) as exc:
            resp.error = f"Anthropic parse error: {exc}\nRaw: {json.dumps(data, ensure_ascii=False)[:500]}"
        return resp

    @staticmethod
    def _parse_responses_response(data: Dict[str, Any], cfg: ProviderConfig) -> LLMResponse:
        """Parse OpenAI Responses API non-streaming response."""
        resp = LLMResponse(model=data.get("model", cfg.effective_model))
        try:
            resp.finish_reason = data.get("status", "")
            err = data.get("error")
            if err:
                resp.error = err.get("message", str(err)) if isinstance(err, dict) else str(err)
            output_text = data.get("output_text", "") or ""
            if output_text:
                resp.content = output_text
            include_item_text = not bool(output_text)
            for item in data.get("output", []):
                LLMEngine._merge_responses_output_item(resp, item, include_item_text)
            usage = data.get("usage", {})
            if usage:
                resp.usage = LLMEngine._normalize_usage(usage)
        except (KeyError, TypeError) as exc:
            resp.error = f"Responses parse error: {exc}\nRaw: {json.dumps(data, ensure_ascii=False)[:500]}"
        return resp

    @staticmethod
    def _merge_responses_output_item(
        resp: LLMResponse,
        item: Dict[str, Any],
        include_text: bool,
    ) -> None:
        itype = item.get("type", "")
        if itype == "message":
            for part in item.get("content", []):
                if not isinstance(part, dict):
                    continue
                ptype = part.get("type", "")
                if ptype in {"output_text", "text"} and include_text:
                    resp.content += part.get("text", "") or ""
                elif ptype == "refusal":
                    resp.refusal = (resp.refusal or "") + (part.get("refusal") or part.get("text", "") or "")
                elif ptype in {"reasoning_text", "summary_text"}:
                    resp.thinking += part.get("text", "") or ""
        elif itype == "function_call":
            resp.tool_calls.append(ToolCall(
                id=item.get("call_id") or item.get("id", ""),
                name=item.get("name", ""),
                arguments=LLMEngine._coerce_tool_arguments(item.get("arguments", "")),
            ))
        elif itype == "reasoning":
            for part in item.get("summary", []):
                if isinstance(part, dict):
                    resp.thinking += part.get("text", "") or ""

    @staticmethod
    def _parse_response(data: Dict[str, Any], cfg: ProviderConfig) -> LLMResponse:
        resp = LLMResponse(model=cfg.effective_model)
        try:
            choice = data["choices"][0]
            msg = choice.get("message", {})
            resp.content = msg.get("content", "") or ""
            resp.thinking = msg.get("reasoning_content", "") or ""
            resp.finish_reason = choice.get("finish_reason", "")
            # Handle refusal field (e.g. OpenAI content filtering)
            refusal = msg.get("refusal")
            if refusal:
                resp.refusal = refusal
            for tc in msg.get("tool_calls", []):
                fn = tc.get("function", {})
                resp.tool_calls.append(ToolCall(
                    id=tc.get("id", ""),
                    name=fn.get("name", ""),
                    arguments=LLMEngine._coerce_tool_arguments(fn.get("arguments", "")),
                ))
            if "usage" in data:
                resp.usage = LLMEngine._normalize_usage(data["usage"])
        except (KeyError, IndexError, TypeError) as exc:
            resp.error = f"Parse error: {exc}\nRaw: {json.dumps(data, ensure_ascii=False)[:500]}"
        return resp

    @staticmethod
    def _parse_stream_chunk(chunk: Dict[str, Any]) -> StreamDelta:
        delta = StreamDelta()
        try:
            choice = chunk.get("choices", [{}])[0]
            d = choice.get("delta", {})
            delta.content = d.get("content", "") or ""
            delta.thinking = d.get("reasoning_content", "") or ""
            delta.tool_calls = d.get("tool_calls", [])
            delta.finish_reason = choice.get("finish_reason")
            # Capture refusal from streaming delta
            refusal = d.get("refusal")
            if refusal:
                delta.refusal = refusal
        except (IndexError, TypeError):
            pass
        if "usage" in chunk and chunk["usage"]:
            delta.usage = LLMEngine._normalize_usage(chunk["usage"])
        return delta

    # -- Convenience --

    def test_connection(self, config_override: Optional[ProviderConfig] = None) -> Tuple[bool, str]:
        cfg = config_override or self.config
        try:
            resp = self.chat_completion(
                [{"role": "user", "content": "Say OK"}],
                config_override=cfg,
            )
            if resp.error:
                return False, resp.error
            return True, f"Connected: {cfg.effective_model} — {resp.content[:60]}"
        except Exception as exc:
            return False, str(exc)

    # -- Token counting (estimation) --

    _CJK_RE = None  # lazy-compiled regex

    @staticmethod
    def estimate_tokens(text: str, model: str = "") -> int:
        """Rough token estimate. ~4 chars per token for English, ~2 for CJK.

        Uses len(text)//4 + CJK char count via regex for speed on large texts.
        """
        if not text:
            return 0
        import re
        if LLMEngine._CJK_RE is None:
            LLMEngine._CJK_RE = re.compile(r'[一-鿿　-〿]')
        cjk = len(LLMEngine._CJK_RE.findall(text))
        # Base: len//4 gives English token estimate.
        # CJK chars need ~1/1.5 tokens each instead of ~1/4, so add the
        # difference: cjk * (1/1.5 - 1/4) = cjk * 5/12
        return len(text) // 4 + (cjk * 5 + 6) // 12

    def count_message_tokens(self, messages: List[Dict[str, Any]]) -> int:
        """Estimate total tokens for a message array."""
        total = 0
        for msg in messages:
            content = msg.get("content", "")
            total += self._count_content_tokens(content)
            for key in ("name", "tool_call_id", "call_id", "arguments", "output"):
                if key in msg:
                    total += self._count_payload_tokens(msg.get(key))
            if msg.get("tool_calls"):
                total += self._count_payload_tokens(msg.get("tool_calls"))
            total += 4  # per-message overhead
        return total

    @classmethod
    def _count_content_tokens(cls, content: Any) -> int:
        if isinstance(content, str):
            return cls.estimate_tokens(content)
        if isinstance(content, list):
            total = 0
            for part in content:
                if isinstance(part, dict):
                    ptype = part.get("type")
                    if ptype in {"text", "input_text", "output_text", "summary_text", "reasoning_text"}:
                        total += cls.estimate_tokens(str(part.get("text", "")))
                    elif ptype in {"image_url", "input_image"}:
                        total += 85
                    else:
                        total += cls._count_payload_tokens(part)
                else:
                    total += cls._count_payload_tokens(part)
            return total
        return cls._count_payload_tokens(content)

    @classmethod
    def _count_payload_tokens(cls, value: Any) -> int:
        if value is None:
            return 0
        if isinstance(value, str):
            if value.startswith("data:image/"):
                return 85
            return cls.estimate_tokens(value)
        if isinstance(value, dict):
            total = 0
            for key, item in value.items():
                if key in {"image_url", "url"} and isinstance(item, str) and item.startswith("data:image/"):
                    total += 85
                else:
                    total += cls.estimate_tokens(str(key)) + cls._count_payload_tokens(item)
            return total
        if isinstance(value, list):
            return sum(cls._count_payload_tokens(item) for item in value)
        return cls.estimate_tokens(str(value))

    # -- Image/vision message helpers --

    @staticmethod
    def make_image_content(text: str, image_base64: str, mime: str = "image/png") -> List[Dict[str, Any]]:
        """Build a multimodal content array with text + image."""
        parts: List[Dict[str, Any]] = []
        if text:
            parts.append({"type": "text", "text": text})
        parts.append({
            "type": "image_url",
            "image_url": {"url": f"data:{mime};base64,{image_base64}"},
        })
        return parts

    @staticmethod
    def make_image_content_anthropic(text: str, image_base64: str, mime: str = "image/png") -> List[Dict[str, Any]]:
        """Build Anthropic-native multimodal content."""
        parts: List[Dict[str, Any]] = []
        if text:
            parts.append({"type": "text", "text": text})
        parts.append({
            "type": "image",
            "source": {"type": "base64", "media_type": mime, "data": image_base64},
        })
        return parts
