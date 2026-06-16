"""Multi-provider LLM engine with streaming and tool-calling support.

Targets the OpenAI chat-completions wire format, which is also spoken by
Anthropic (via proxy), DeepSeek, Ollama, vLLM, and any compatible endpoint.
"""

from __future__ import annotations

import json
import threading
import time
import uuid
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
        "default_model": "claude-sonnet-4-20250514",
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
    tool_calls: List[Dict[str, Any]] = field(default_factory=list)
    finish_reason: Optional[str] = None
    usage: Optional[Dict[str, int]] = None


@dataclass
class LLMResponse:
    content: str = ""
    tool_calls: List[ToolCall] = field(default_factory=list)
    finish_reason: str = ""
    usage: Dict[str, int] = field(default_factory=dict)
    error: Optional[str] = None
    model: str = ""


@dataclass
class ProviderConfig:
    provider: str = Provider.OPENAI.value
    api_key: str = ""
    base_url: str = ""
    model: str = ""
    temperature: float = 0.7
    max_tokens: int = 4096
    system_prompt: str = ""
    extra_headers: Dict[str, str] = field(default_factory=dict)

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


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------

class LLMEngine:
    """Stateless LLM caller: given config + messages + tools → response / stream."""

    def __init__(self, config: Optional[ProviderConfig] = None):
        self.config = config or ProviderConfig()
        self._cancel = threading.Event()

    def cancel(self) -> None:
        self._cancel.set()

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
        body = self._build_body(cfg, messages, tools, stream=False)
        try:
            import httpx
            headers = self._build_headers(cfg)
            if self._is_anthropic_native(cfg):
                url = f"{cfg.effective_base_url}/messages"
            else:
                url = f"{cfg.effective_base_url}/chat/completions"
            with httpx.Client(timeout=120.0) as client:
                resp = client.post(url, json=body, headers=headers)
                resp.raise_for_status()
                data = resp.json()
                if self._is_anthropic_native(cfg):
                    return self._parse_anthropic_response(data, cfg)
                return self._parse_response(data, cfg)
        except Exception as exc:
            return LLMResponse(error=str(exc))

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
        self._cancel.clear()
        cfg = config_override or self.config

        if self._is_anthropic_native(cfg):
            return self._stream_anthropic_native(cfg, messages, tools, on_delta)

        return self._stream_openai(cfg, messages, tools, on_delta)

    # -- OpenAI-compatible SSE streaming --

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

        try:
            import httpx
            headers = self._build_headers(cfg)
            url = f"{cfg.effective_base_url}/chat/completions"

            with httpx.Client(timeout=180.0) as client:
                with client.stream("POST", url, json=body, headers=headers) as resp:
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
                        if delta.finish_reason:
                            accumulated.finish_reason = delta.finish_reason
                        if delta.usage:
                            accumulated.usage = delta.usage

                        for tc_delta in delta.tool_calls:
                            idx = tc_delta.get("index", 0)
                            if idx not in tool_call_buffers:
                                tool_call_buffers[idx] = {
                                    "id": tc_delta.get("id", f"call_{uuid.uuid4().hex[:8]}"),
                                    "name": "",
                                    "arguments": "",
                                }
                            buf = tool_call_buffers[idx]
                            fn = tc_delta.get("function", {})
                            if fn.get("name"):
                                buf["name"] = fn["name"]
                            if fn.get("arguments"):
                                buf["arguments"] += fn["arguments"]

                        if on_delta:
                            on_delta(delta)

        except Exception as exc:
            accumulated.error = str(exc)

        for _idx in sorted(tool_call_buffers):
            buf = tool_call_buffers[_idx]
            accumulated.tool_calls.append(
                ToolCall(id=buf["id"], name=buf["name"], arguments=buf["arguments"])
            )

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

        try:
            import httpx
            headers = self._build_headers(cfg)
            url = f"{cfg.effective_base_url}/messages"

            with httpx.Client(timeout=180.0) as client:
                with client.stream("POST", url, json=body, headers=headers) as resp:
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
                                accumulated.usage["input_tokens"] = usage.get("input_tokens", 0)

                        elif dtype == "content_block_start":
                            idx = data.get("index", 0)
                            block = data.get("content_block", {})
                            if block.get("type") == "tool_use":
                                tool_blocks[idx] = {
                                    "id": block.get("id", f"toolu_{uuid.uuid4().hex[:8]}"),
                                    "name": block.get("name", ""),
                                    "input_json": "",
                                }

                        elif dtype == "content_block_delta":
                            idx = data.get("index", 0)
                            d = data.get("delta", {})
                            if d.get("type") == "text_delta":
                                text = d.get("text", "")
                                if text:
                                    accumulated.content += text
                                    if on_delta:
                                        on_delta(StreamDelta(content=text))
                            elif d.get("type") == "input_json_delta":
                                partial = d.get("partial_json", "")
                                if idx in tool_blocks:
                                    tool_blocks[idx]["input_json"] += partial

                        elif dtype == "message_delta":
                            d = data.get("delta", {})
                            accumulated.finish_reason = d.get("stop_reason", "")
                            usage = data.get("usage", {})
                            if usage:
                                accumulated.usage["output_tokens"] = usage.get("output_tokens", 0)
                                inp = accumulated.usage.get("input_tokens", 0)
                                out = usage.get("output_tokens", 0)
                                accumulated.usage["total_tokens"] = inp + out

                        elif dtype == "message_stop":
                            break

                        elif dtype == "error":
                            err = data.get("error", {})
                            accumulated.error = err.get("message", str(data))
                            break

        except Exception as exc:
            accumulated.error = str(exc)

        for _idx in sorted(tool_blocks):
            tb = tool_blocks[_idx]
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
        if cfg.max_tokens > 0:
            body["max_tokens"] = cfg.max_tokens
        if tools:
            body["tools"] = tools
        if stream:
            body["stream_options"] = {"include_usage": True}
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
        if tools:
            body["tools"] = [self._convert_tool_to_anthropic(t) for t in tools]
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
                try:
                    inp = json.loads(fn.get("arguments", "{}"))
                except json.JSONDecodeError:
                    inp = {}
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

    # -- Response parsing --

    @staticmethod
    def _parse_anthropic_response(data: Dict[str, Any], cfg: ProviderConfig) -> LLMResponse:
        """Parse Anthropic Messages API non-streaming response."""
        resp = LLMResponse(model=data.get("model", cfg.effective_model))
        try:
            resp.finish_reason = data.get("stop_reason", "")
            usage = data.get("usage", {})
            if usage:
                resp.usage = {
                    "input_tokens": usage.get("input_tokens", 0),
                    "output_tokens": usage.get("output_tokens", 0),
                    "total_tokens": usage.get("input_tokens", 0) + usage.get("output_tokens", 0),
                }
            for block in data.get("content", []):
                btype = block.get("type", "")
                if btype == "text":
                    resp.content += block.get("text", "")
                elif btype == "tool_use":
                    inp = block.get("input", {})
                    resp.tool_calls.append(ToolCall(
                        id=block.get("id", ""),
                        name=block.get("name", ""),
                        arguments=json.dumps(inp, ensure_ascii=False) if isinstance(inp, dict) else str(inp),
                    ))
        except (KeyError, TypeError) as exc:
            resp.error = f"Anthropic parse error: {exc}\nRaw: {json.dumps(data, ensure_ascii=False)[:500]}"
        return resp

    @staticmethod
    def _parse_response(data: Dict[str, Any], cfg: ProviderConfig) -> LLMResponse:
        resp = LLMResponse(model=cfg.effective_model)
        try:
            choice = data["choices"][0]
            msg = choice.get("message", {})
            resp.content = msg.get("content", "") or ""
            resp.finish_reason = choice.get("finish_reason", "")
            for tc in msg.get("tool_calls", []):
                fn = tc.get("function", {})
                resp.tool_calls.append(ToolCall(
                    id=tc.get("id", ""),
                    name=fn.get("name", ""),
                    arguments=fn.get("arguments", ""),
                ))
            if "usage" in data:
                resp.usage = data["usage"]
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
            delta.tool_calls = d.get("tool_calls", [])
            delta.finish_reason = choice.get("finish_reason")
        except (IndexError, TypeError):
            pass
        if "usage" in chunk and chunk["usage"]:
            delta.usage = chunk["usage"]
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
