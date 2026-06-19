"""WebView bridge endpoints for the AI Editor.

Registers command handlers on the C#/pywebview bridge so that
``ai_editor.html`` can communicate with the Python backend.

Bridge protocol:
  JS  → Python:  bridge.cmd('ai_editor_*', payload) → reply
  Python → JS:   bridge event 'ai_editor_*' with payload
"""

from __future__ import annotations

import json
import logging
import threading
from typing import Any, Callable, Dict, List, Optional

from ai_editor.llm_engine import LLMEngine, ProviderConfig, StreamDelta
from ai_editor.tool_registry import ToolRegistry
from ai_editor.engine_tools import register_engine_tools
from ai_editor.chat_state import ChatController, Conversation, ChatMessage


logger = logging.getLogger(__name__)


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
        self._controller.on_thinking_delta = self._on_thinking_delta
        self._controller.on_stream_end = self._on_stream_end
        self._controller.on_tool_start = self._on_tool_start
        self._controller.on_tool_end = self._on_tool_end
        self._controller.on_tool_confirm = self._on_tool_confirm
        self._controller.on_tool_progress = self._on_tool_progress
        self._controller.on_error = self._on_error
        self._controller.on_idle = self._on_idle

    def _settings_getter(self, key: str, default=None):
        settings = getattr(self._gui_ref, 'settings', None)
        if settings:
            return settings.get(key, default)
        return default

    def _default_system_prompt(self) -> str:
        from ai_editor.prompts import get_system_prompt
        return get_system_prompt(settings_getter=self._settings_getter)

    def _current_mode(self) -> str:
        from ai_editor.scopes import MODES, normalize_mode
        ai = self._settings_getter("ai_editor", {}) or {}
        raw_mode = ai.get("mode", "agent") if isinstance(ai, dict) else "agent"
        mode = normalize_mode(str(raw_mode or "agent"))
        return mode if mode in MODES else "agent"

    def _mode_system_prompt(self, mode: Optional[str] = None) -> str:
        from ai_editor.prompts import get_system_prompt
        selected_mode = mode or self._current_mode()
        custom = ""
        if self._engine and self._engine.config.system_prompt:
            custom = self._engine.config.system_prompt
        return get_system_prompt(
            agent_mode=selected_mode == "agent",
            plan_mode=selected_mode == "plan",
            custom=custom,
            settings_getter=self._settings_getter,
        )

    def _refresh_system_prompt(self, mode: Optional[str] = None) -> None:
        if self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._mode_system_prompt(mode)

    def _save_ai_settings_patch(self, patch: Dict[str, Any]) -> Dict[str, Any]:
        settings = getattr(self._gui_ref, 'settings', None)
        if not settings:
            raise RuntimeError("Settings not available")
        from ai_editor.app import _merge_ai_editor_config
        current = settings.get("ai_editor", {}) or {}
        merged = _merge_ai_editor_config(current, patch)
        settings.set("ai_editor", merged)
        settings.save()
        return merged

    def _load_config(self) -> ProviderConfig:
        from ai_editor.app import load_provider_config
        return load_provider_config(self._gui_ref)

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
            "ai_editor_get_instructions": self._cmd_get_instructions,
            "ai_editor_save_user_instructions": self._cmd_save_user_instructions,
            "ai_editor_get_instruction_files": self._cmd_get_instruction_files,
            "ai_editor_save_instruction_file": self._cmd_save_instruction_file,
            "ai_editor_delete_instruction_file": self._cmd_delete_instruction_file,
            "ai_editor_list_agents": self._cmd_list_agents,
            "ai_editor_save_agent": self._cmd_save_agent,
            "ai_editor_delete_agent": self._cmd_delete_agent,
            "ai_editor_list_workflows": self._cmd_list_workflows,
            "ai_editor_save_workflow": self._cmd_save_workflow,
            "ai_editor_delete_workflow": self._cmd_delete_workflow,
            "ai_editor_count_tokens": self._cmd_count_tokens,
            "ai_editor_get_model_info": self._cmd_get_model_info,
            "ai_editor_get_mode": self._cmd_get_mode,
            "ai_editor_set_mode": self._cmd_set_mode,
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
        config_patch = {
            "provider": payload.get("provider", "openai"),
            "api_key": payload.get("api_key", ""),
            "base_url": payload.get("base_url", ""),
            "model": payload.get("model", ""),
            "temperature": payload.get("temperature", 0.7),
            "max_tokens": payload.get("max_tokens", 4096),
            "system_prompt": payload.get("system_prompt", ""),
        }
        warning = ""
        try:
            merged = self._save_ai_settings_patch(config_patch)
        except Exception as exc:
            logger.warning("Failed to save AI Editor bridge config: %s", exc)
            merged = config_patch
            warning = str(exc)
        if self._engine:
            self._engine.config = ProviderConfig(**{
                k: v for k, v in merged.items()
                if k in ProviderConfig.__dataclass_fields__
            })
        self._refresh_system_prompt()
        result = {"ok": True}
        if warning:
            result["warning"] = warning
        return result

    def _cmd_send(self, payload: Dict) -> Dict:
        text = payload.get("text", "").strip()
        if not text:
            return {"error": "Empty message"}
        self._ensure_engine()
        mode = self._current_mode()

        config_override = payload.get("config")
        if config_override and isinstance(config_override, dict):
            if config_override.get("provider"):
                self._engine.config.provider = config_override["provider"]
            if config_override.get("model"):
                self._engine.config.model = config_override["model"]
            if config_override.get("system_prompt"):
                self._engine.config.system_prompt = config_override["system_prompt"]

        self._refresh_system_prompt(mode)
        self._controller.send(text, agent_mode=mode == "agent")
        return {"ok": True, "mode": mode}

    def _cmd_cancel(self, payload: Dict) -> Dict:
        if self._controller:
            self._controller.cancel()
        return {"ok": True}

    def _cmd_new_chat(self, payload: Dict) -> Dict:
        if self._controller:
            mode = self._current_mode()
            self._controller.new_conversation(self._mode_system_prompt(mode))
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

    # ── Agents & Workflows ──

    def _cmd_list_agents(self, payload: Dict) -> Dict:
        from ai_editor.agents import get_agent_registry
        return {"agents": [a.to_dict() for a in get_agent_registry().list_all()]}

    def _cmd_save_agent(self, payload: Dict) -> Dict:
        from ai_editor.agents import AgentDef, get_agent_registry
        agent = AgentDef.from_dict(payload)
        return get_agent_registry().save_custom(agent)

    def _cmd_delete_agent(self, payload: Dict) -> Dict:
        from ai_editor.agents import get_agent_registry
        return get_agent_registry().delete_custom(payload.get("id", ""))

    def _cmd_list_workflows(self, payload: Dict) -> Dict:
        from ai_editor.workflows import get_workflow_registry
        return {"workflows": [w.to_dict() for w in get_workflow_registry().list_all()]}

    def _cmd_save_workflow(self, payload: Dict) -> Dict:
        from ai_editor.workflows import WorkflowDef, get_workflow_registry
        wf = WorkflowDef.from_dict(payload)
        return get_workflow_registry().save_custom(wf)

    def _cmd_delete_workflow(self, payload: Dict) -> Dict:
        from ai_editor.workflows import get_workflow_registry
        return get_workflow_registry().delete_custom(payload.get("id", ""))

    # ── Instructions ──

    def _cmd_get_instructions(self, payload: Dict) -> Dict:
        from ai_editor.prompts import load_instructions, list_instruction_files
        ai = self._settings_getter("ai_editor", {}) or {}
        user_text = ai.get("user_instructions", "") if isinstance(ai, dict) else ""
        files = list_instruction_files()
        combined = load_instructions(settings_getter=self._settings_getter)
        return {"user_instructions": user_text, "files": files,
                "combined_preview": combined}

    def _cmd_save_user_instructions(self, payload: Dict) -> Dict:
        settings = getattr(self._gui_ref, 'settings', None)
        if not settings:
            return {"error": "Settings not available"}
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["user_instructions"] = payload.get("text", "")
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception as exc:
            logger.warning("Failed to save user instructions: %s", exc)
            return {"error": str(exc)}
        self._refresh_system_prompt()
        return {"ok": True}

    def _cmd_get_instruction_files(self, payload: Dict) -> Dict:
        from ai_editor.prompts import list_instruction_files
        return {"files": list_instruction_files()}

    def _cmd_save_instruction_file(self, payload: Dict) -> Dict:
        from ai_editor.prompts import save_instruction_file
        result = save_instruction_file(payload.get("name", ""),
                                       payload.get("content", ""))
        if result.get("ok"):
            self._refresh_system_prompt()
        return result

    def _cmd_delete_instruction_file(self, payload: Dict) -> Dict:
        from ai_editor.prompts import delete_instruction_file
        result = delete_instruction_file(payload.get("name", ""))
        if result.get("ok"):
            self._refresh_system_prompt()
        return result

    # ── Token / Model / Mode bridge commands ──

    def _cmd_count_tokens(self, payload: Dict) -> Dict:
        self._ensure_engine()
        text = payload.get("text", "")
        count = self._engine.estimate_tokens(text) if self._engine else len(text) // 4
        return {"tokens": count}

    def _cmd_get_model_info(self, payload: Dict) -> Dict:
        self._ensure_engine()
        from ai_editor.llm_engine import get_model_context, compaction_threshold
        model = payload.get("model", "") or (self._engine.config.effective_model if self._engine else "")
        ctx = get_model_context(model)
        return {"model": model, "max_input": ctx["max_input"],
                "max_output": ctx["max_output"],
                "compact_at": compaction_threshold(model)}

    def _cmd_get_mode(self, payload: Dict) -> Dict:
        self._ensure_engine()
        from ai_editor.scopes import MODES, MODE_PERMISSIONS, normalize_mode, tool_permission
        ai = self._settings_getter("ai_editor", {}) or {}
        overrides = ai.get("permissions", {}) if isinstance(ai, dict) else {}
        requested = normalize_mode(str(payload.get("mode") or "")) if isinstance(payload, dict) else ""
        selected = requested if requested in MODES else self._current_mode()
        tool_names = set(MODE_PERMISSIONS.get(selected, MODE_PERMISSIONS["agent"]).keys())
        registry_tools = {}
        if self._registry:
            for tool in self._registry.list_tools(include_disabled=True):
                registry_tools[tool.name] = tool
                tool_names.add(tool.name)
        permissions: Dict[str, str] = {}
        defaults: Dict[str, str] = {}
        for tool_name in sorted(tool_names):
            tool = registry_tools.get(tool_name)
            read_only = None
            category = ""
            if tool is not None:
                category = getattr(tool, "category", "")
                tags = getattr(tool, "tags", {}) or {}
                if isinstance(tags, dict) and isinstance(tags.get("readOnly"), bool):
                    read_only = tags.get("readOnly")
            permissions[tool_name] = tool_permission(
                selected, tool_name, overrides,
                read_only=read_only, category=category,
            )
            defaults[tool_name] = tool_permission(
                selected, tool_name, {},
                read_only=read_only, category=category,
            )
        return {
            "mode": self._current_mode(),
            "selected_mode": selected,
            "modes": list(MODES),
            "permissions": permissions,
            "defaults": defaults,
            "overrides": dict(overrides) if isinstance(overrides, dict) else {},
            "tools": sorted(tool_names),
        }

    def _cmd_set_mode(self, payload: Dict) -> Dict:
        from ai_editor.scopes import MODES, normalize_mode
        requested = normalize_mode(str(payload.get("mode") or ""))
        if requested not in MODES:
            return {"error": f"Invalid mode: {requested}. Valid: {list(MODES)}"}
        try:
            self._save_ai_settings_patch({"mode": requested})
        except Exception as exc:
            logger.warning("Failed to save AI Editor mode: %s", exc)
            return {"error": str(exc)}
        self._refresh_system_prompt(requested)
        return {"ok": True, "mode": requested}

    # ── Event emitters (called from background thread) ──

    def _on_stream_delta(self, msg: ChatMessage, text: str) -> None:
        self._emit("ai_editor_stream_delta", {"content": text})

    def _on_thinking_delta(self, msg: ChatMessage, text: str) -> None:
        self._emit("ai_editor_thinking_delta", {"content": text})

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

    def _on_tool_start(self, call_id: str, name: str, args: str, state: str = "") -> None:
        self._emit("ai_editor_tool_start", {"id": call_id, "name": name, "arguments": args, "state": state})

    def _on_tool_end(self, call_id: str, result: str, state: str = "") -> None:
        self._emit("ai_editor_tool_end", {"id": call_id, "result": result, "state": state})

    def _on_tool_confirm(self, call_id: str, name: str, args: str) -> bool:
        self._emit("ai_editor_tool_confirm", {"id": call_id, "name": name, "arguments": args})
        return True

    def _on_tool_progress(self, call_id: str, name: str, progress: float) -> None:
        self._emit("ai_editor_tool_progress", {"id": call_id, "name": name, "progress": progress})

    def _on_error(self, error: str) -> None:
        self._emit("ai_editor_error", {"error": error})

    def _on_idle(self) -> None:
        self._emit("ai_editor_idle", {})
