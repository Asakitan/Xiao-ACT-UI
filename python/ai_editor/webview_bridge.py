"""WebView bridge endpoints for the AI Editor.

Registers command handlers on the C#/pywebview bridge so that
``ai_editor.html`` can communicate with the Python backend.

Bridge protocol:
  JS  → Python:  bridge.cmd('ai_editor_*', payload) → reply
  Python → JS:   bridge event 'ai_editor_*' with payload
"""

from __future__ import annotations

import json
import threading
from typing import Any, Callable, Dict, List, Optional

from ai_editor.llm_engine import LLMEngine, ProviderConfig, StreamDelta
from ai_editor.tool_registry import ToolRegistry
from ai_editor.engine_tools import register_engine_tools
from ai_editor.chat_state import ChatController, Conversation, ChatMessage


class AIEditorBridge:
    """Manages the AI editor session for a single WebView window."""

    def __init__(self, gui_ref: Any, emit: Callable[[str, Any], None]) -> None:
        """
        Args:
            gui_ref: The SAOPlayerGUI / SAOWebViewGUI instance.
            emit:    ``emit(event_name, payload)`` pushes an event to JS.
        """
        self._gui_ref = gui_ref
        self._emit = emit
        self._engine: Optional[LLMEngine] = None
        self._registry: Optional[ToolRegistry] = None
        self._controller: Optional[ChatController] = None

    def _ensure_engine(self) -> None:
        if self._controller is not None:
            return
        config = self._load_config()
        self._engine = LLMEngine(config)
        self._registry = ToolRegistry()
        register_engine_tools(self._registry, self._gui_ref)

        system_prompt = config.system_prompt or self._default_system_prompt()
        conv = Conversation(system_prompt=system_prompt)
        self._controller = ChatController(self._engine, self._registry, conv)

        self._controller.on_stream_delta = self._on_stream_delta
        self._controller.on_stream_end = self._on_stream_end
        self._controller.on_tool_start = self._on_tool_start
        self._controller.on_tool_end = self._on_tool_end
        self._controller.on_error = self._on_error
        self._controller.on_idle = self._on_idle

    def _default_system_prompt(self) -> str:
        tools_desc = ""
        if self._registry:
            for cat in self._registry.categories():
                names = [t.name for t in self._registry.list_tools(cat)]
                tools_desc += f"\n- {cat}: {', '.join(names)}"
        return (
            "你是 SAO ACT UI 的 AI 助手。\n"
            "你可以通过 tool call 访问平台和插件暴露的运行时接口。\n"
            f"\n可用工具分类:{tools_desc}"
        )

    def _load_config(self) -> ProviderConfig:
        settings = getattr(self._gui_ref, 'settings', None)
        if not settings:
            return ProviderConfig()
        raw = settings.get("ai_editor", {}) or {}
        return ProviderConfig(
            provider=raw.get("provider", "openai"),
            api_key=raw.get("api_key", ""),
            base_url=raw.get("base_url", ""),
            model=raw.get("model", ""),
            temperature=raw.get("temperature", 0.7),
            max_tokens=raw.get("max_tokens", 4096),
            system_prompt=raw.get("system_prompt", ""),
        )

    # ── Bridge command handlers ──

    def handle_command(self, name: str, payload: Any) -> Any:
        """Dispatch a bridge command and return the reply payload."""
        handlers = {
            "ai_editor_load_config": self._cmd_load_config,
            "ai_editor_save_config": self._cmd_save_config,
            "ai_editor_send": self._cmd_send,
            "ai_editor_cancel": self._cmd_cancel,
            "ai_editor_new_chat": self._cmd_new_chat,
            "ai_editor_export": self._cmd_export,
            "ai_editor_list_tools": self._cmd_list_tools,
            "ai_editor_test_connection": self._cmd_test_connection,
            "ai_editor_list_history": self._cmd_list_history,
            "ai_editor_load_history": self._cmd_load_history,
        }
        handler = handlers.get(name)
        if handler:
            return handler(payload or {})
        return {"error": f"Unknown command: {name}"}

    def _cmd_load_config(self, payload: Dict) -> Dict:
        config = self._load_config()
        return {
            "provider": config.provider,
            "api_key": config.api_key,
            "base_url": config.base_url,
            "model": config.model or config.effective_model,
            "temperature": config.temperature,
            "max_tokens": config.max_tokens,
            "system_prompt": config.system_prompt,
        }

    def _cmd_save_config(self, payload: Dict) -> Dict:
        settings = getattr(self._gui_ref, 'settings', None)
        if not settings:
            return {"error": "Settings not available"}
        settings.set("ai_editor", {
            "provider": payload.get("provider", "openai"),
            "api_key": payload.get("api_key", ""),
            "base_url": payload.get("base_url", ""),
            "model": payload.get("model", ""),
            "temperature": payload.get("temperature", 0.7),
            "max_tokens": payload.get("max_tokens", 4096),
            "system_prompt": payload.get("system_prompt", ""),
        })
        try:
            settings.save()
        except Exception:
            pass
        if self._engine:
            self._engine.config = ProviderConfig(**{k: v for k, v in payload.items()
                                                    if k in ProviderConfig.__dataclass_fields__})
        return {"ok": True}

    def _cmd_send(self, payload: Dict) -> Dict:
        text = payload.get("text", "").strip()
        if not text:
            return {"error": "Empty message"}
        self._ensure_engine()

        config_override = payload.get("config")
        if config_override and isinstance(config_override, dict):
            if config_override.get("provider"):
                self._engine.config.provider = config_override["provider"]
            if config_override.get("model"):
                self._engine.config.model = config_override["model"]

        self._controller.send(text)
        return {"ok": True}

    def _cmd_cancel(self, payload: Dict) -> Dict:
        if self._controller:
            self._controller.cancel()
        return {"ok": True}

    def _cmd_new_chat(self, payload: Dict) -> Dict:
        if self._controller:
            sp = self._engine.config.system_prompt if self._engine else ""
            self._controller.new_conversation(sp or self._default_system_prompt())
        return {"ok": True}

    def _cmd_export(self, payload: Dict) -> Any:
        if not self._controller:
            return None
        return self._controller.export_messages()

    def _cmd_list_tools(self, payload: Dict) -> Dict:
        self._ensure_engine()
        tools = []
        for t in self._registry.list_tools():
            tools.append({
                "name": t.name,
                "description": t.description,
                "category": t.category,
                "requires_confirm": t.requires_confirm,
            })
        return {"tools": tools}

    def _cmd_test_connection(self, payload: Dict) -> Dict:
        self._ensure_engine()
        test_cfg = ProviderConfig(
            provider=payload.get("provider", "openai"),
            api_key=payload.get("api_key", ""),
            base_url=payload.get("base_url", ""),
            model=payload.get("model", ""),
        )
        ok, msg = self._engine.test_connection(test_cfg)
        return {"ok": ok, "message": msg}

    def _cmd_list_history(self, payload: Dict) -> Dict:
        try:
            from ai_editor.history import list_conversations
            return {"entries": list_conversations(limit=30)}
        except Exception as exc:
            return {"error": str(exc)}

    def _cmd_load_history(self, payload: Dict) -> Dict:
        conv_id = payload.get("id", "")
        if not conv_id:
            return {"error": "Missing conversation id"}
        try:
            from ai_editor.history import load_conversation
            data = load_conversation(conv_id)
            if data:
                return data
            return {"error": "Not found"}
        except Exception as exc:
            return {"error": str(exc)}

    # ── Event emitters (called from background thread) ──

    def _on_stream_delta(self, msg: ChatMessage, text: str) -> None:
        self._emit("ai_editor_stream_delta", {"content": text})

    def _on_stream_end(self, msg: ChatMessage) -> None:
        payload: Dict[str, Any] = {"content": msg.content}
        if msg.tool_calls:
            payload["tool_calls"] = [
                {"id": tc.id, "name": tc.name, "arguments": tc.arguments}
                for tc in msg.tool_calls
            ]
        if msg.is_error:
            payload["error"] = msg.content
        if msg.usage:
            payload["usage"] = msg.usage
        self._emit("ai_editor_stream_end", payload)

    def _on_tool_start(self, call_id: str, name: str, args: str) -> None:
        self._emit("ai_editor_tool_start", {"id": call_id, "name": name, "arguments": args})

    def _on_tool_end(self, call_id: str, result: str) -> None:
        self._emit("ai_editor_tool_end", {"id": call_id, "result": result})

    def _on_error(self, error: str) -> None:
        self._emit("ai_editor_error", {"error": error})

    def _on_idle(self) -> None:
        self._emit("ai_editor_idle", {})
