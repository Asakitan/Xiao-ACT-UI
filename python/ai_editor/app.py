"""SAO AI Editor — standalone pywebview GUI application.

Launch:
    python -m ai_editor.app            # standalone
    python -m ai_editor.app --attach   # attached to running SAO instance

From SAO menu, ``_toggle_ai_editor_panel`` calls ``launch()`` which opens
the pywebview window in a background thread if not already running.
"""

from __future__ import annotations

import json
import math
import os
import sys
import threading
import time
from typing import Any, Callable, Dict, List, Optional

# Ensure package root on path
_HERE = os.path.dirname(__file__)
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ai_editor.llm_engine import LLMEngine, ProviderConfig, StreamDelta
from ai_editor.tool_registry import ToolRegistry
from ai_editor.engine_tools import register_engine_tools
from ai_editor.chat_state import ChatController, Conversation, ChatMessage


# ---------------------------------------------------------------------------
# Settings helpers
# ---------------------------------------------------------------------------

_PROVIDER_CONFIG_KEYS = {
    "provider", "api_key", "base_url", "model", "temperature", "max_tokens",
    "system_prompt", "transport", "top_p", "frequency_penalty", "presence_penalty", "stop",
    "max_input_tokens", "max_output_tokens", "timeout", "extra_headers", "extra_body",
}

_TRANSIENT_CONFIG_KEYS = {"_provider_keys", "context_window"}

_MODE_VALUES = {"chat", "edit", "agent"}
_ENGINE_TRANSPORT_VALUES = {"chat_completions", "responses"}
_CODEX_TRANSPORT_VALUES = {"chat_completions", "responses", "cli"}

_AI_EDITOR_SECTION_DEFAULTS: Dict[str, Dict[str, Any]] = {
    "claude_code": {
        "cli_path": "",
        "cli_args": [],
        "prefer_cli": False,
        "allow_dangerously_skip_permissions": False,
        "model": "claude-sonnet-4-20250514",
    },
    "codex": {
        "cli_path": "",
        "cli_args": [],
        "transport": "chat_completions",
        "model": "codex-mini-latest",
    },
    "mcp": {
        "access": "prompt",
        "autostart": False,
        "discovery_enabled": True,
        "collision_behavior": "first",
        "server_sampling": False,
    },
    "terminal": {
        "profile": "PowerShell 7 (No Profile)",
        "shell_path": "",
        "shell_args": [],
        "timeout": 30,
        "output_limit": 8000,
        "auto_approve": {},
    },
    "extensions": {
        "confirm_install": True,
        "allowed_publishers": [],
        "blocked_publishers": [],
        "enabled_contributions": ["chatParticipants", "languageModelTools", "commands"],
    },
    "customization": {
        "instructions_locations": [".sao/instructions.md", ".sao/instructions"],
        "agent_locations": [".sao/agents"],
        "workflow_locations": [".sao/workflows"],
        "skill_locations": [".agents/skills/.local", ".claude/skills/.local"],
        "use_agent_md": True,
        "use_claude_md": False,
    },
}


def _as_dict(value: Any) -> Dict[str, Any]:
    return dict(value) if isinstance(value, dict) else {}


def _as_list(value: Any) -> List[Any]:
    return list(value) if isinstance(value, list) else []


def _as_float(value: Any, default: float) -> float:
    try:
        number = float(value)
        return number if math.isfinite(number) else default
    except (TypeError, ValueError):
        return default


def _as_int(value: Any, default: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return default


def _normalize_stop(value: Any) -> List[str]:
    if isinstance(value, str):
        return [part.strip() for part in value.split(",") if part.strip()]
    if isinstance(value, list):
        return [str(part) for part in value if str(part)]
    return []


def _normalize_transport(value: Any, default: str = "chat_completions",
                         allowed: Optional[set[str]] = None) -> str:
    allowed_values = allowed or _ENGINE_TRANSPORT_VALUES
    raw = str(value or default).strip().lower().replace("-", "_")
    if raw in {"openai_responses", "response"}:
        raw = "responses"
    return raw if raw in allowed_values else default


def _normalize_ai_editor_config(raw: Any) -> Dict[str, Any]:
    """Return a backward-compatible AI Editor config dict.

    The user settings file can contain older or partially written values. Keep
    unknown keys for forward compatibility, but normalize values consumed by the
    runtime and UI so saving endpoint settings does not corrupt sibling fields.
    """
    cfg = dict(raw) if isinstance(raw, dict) else {}
    if ("provider_keys" not in cfg or not isinstance(cfg.get("provider_keys"), dict)):
        legacy_provider_keys = cfg.get("_provider_keys")
        if isinstance(legacy_provider_keys, dict):
            cfg["provider_keys"] = dict(legacy_provider_keys)
    cfg["provider"] = str(cfg.get("provider") or "openai")
    cfg["api_key"] = str(cfg.get("api_key") or "")
    cfg["base_url"] = str(cfg.get("base_url") or "")
    cfg["model"] = str(cfg.get("model") or "")
    cfg["temperature"] = _as_float(cfg.get("temperature"), 0.7)
    cfg["max_tokens"] = _as_int(cfg.get("max_tokens"), 4096)
    cfg["system_prompt"] = str(cfg.get("system_prompt") or "")
    cfg["transport"] = _normalize_transport(cfg.get("transport"))
    cfg["top_p"] = _as_float(cfg.get("top_p"), 1.0)
    cfg["frequency_penalty"] = _as_float(cfg.get("frequency_penalty"), 0.0)
    cfg["presence_penalty"] = _as_float(cfg.get("presence_penalty"), 0.0)
    cfg["stop"] = _normalize_stop(cfg.get("stop"))
    cfg["max_input_tokens"] = _as_int(cfg.get("max_input_tokens"), 0)
    cfg["max_output_tokens"] = _as_int(cfg.get("max_output_tokens"), 0)
    cfg["timeout"] = _as_int(cfg.get("timeout"), 180)
    cfg["extra_headers"] = _as_dict(cfg.get("extra_headers"))
    cfg["extra_body"] = _as_dict(cfg.get("extra_body"))
    cfg["provider_keys"] = _as_dict(cfg.get("provider_keys"))
    cfg["custom_models"] = _as_dict(cfg.get("custom_models"))
    cfg["permissions"] = _as_dict(cfg.get("permissions"))
    if "mode" in cfg:
        mode = str(cfg.get("mode") or "edit").strip().lower()
        cfg["mode"] = mode if mode in _MODE_VALUES else "edit"
    if "theme" in cfg:
        cfg["theme"] = str(cfg.get("theme") or "")
    for section, defaults in _AI_EDITOR_SECTION_DEFAULTS.items():
        if section in cfg:
            current = _as_dict(cfg.get(section))
            merged = dict(defaults)
            merged.update(current)
            if section == "codex":
                merged["transport"] = _normalize_transport(
                    merged.get("transport"), "chat_completions", _CODEX_TRANSPORT_VALUES)
            cfg[section] = merged
    return cfg


def _merge_ai_editor_config(existing: Any, incoming: Any) -> Dict[str, Any]:
    merged = dict(existing) if isinstance(existing, dict) else {}
    if isinstance(incoming, dict):
        for key, value in incoming.items():
            if key in _TRANSIENT_CONFIG_KEYS:
                if key == "_provider_keys" and "provider_keys" not in incoming:
                    merged["provider_keys"] = _as_dict(value)
                continue
            merged[key] = value
    return _normalize_ai_editor_config(merged)


def load_provider_config(gui_ref: Any = None) -> ProviderConfig:
    """Shared helper: build a ProviderConfig from settings.

    Used by both ``AIEditorAPI`` (pywebview) and ``AIEditorBridge``
    (C# webview) to avoid duplicated config-parsing logic.
    """
    settings = getattr(gui_ref, 'settings', None) if gui_ref else None
    if not settings:
        return ProviderConfig()
    raw = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
    custom_models = raw.get("custom_models", {})
    if custom_models:
        from ai_editor.llm_engine import set_custom_models
        set_custom_models(custom_models)
    return ProviderConfig(
        provider=raw["provider"],
        api_key=raw["api_key"],
        base_url=raw["base_url"],
        model=raw["model"],
        temperature=raw["temperature"],
        max_tokens=raw["max_tokens"],
        system_prompt=raw["system_prompt"],
        transport=raw["transport"],
        top_p=raw["top_p"],
        frequency_penalty=raw["frequency_penalty"],
        presence_penalty=raw["presence_penalty"],
        stop=raw["stop"],
        max_input_tokens=raw["max_input_tokens"],
        max_output_tokens=raw["max_output_tokens"],
        timeout=raw["timeout"],
        extra_headers=raw["extra_headers"],
        extra_body=raw["extra_body"],
    )


# ---------------------------------------------------------------------------
# JS API exposed to the webview window
# ---------------------------------------------------------------------------

class AIEditorAPI:
    """Python backend exposed to JavaScript via ``window.pywebview.api``."""

    def __init__(self, gui_ref: Any = None) -> None:
        self._gui_ref = gui_ref
        self._engine: Optional[LLMEngine] = None
        self._registry: Optional[ToolRegistry] = None
        self._controller: Optional[ChatController] = None
        self._window = None  # set after window creation
        self._ready = threading.Event()
        self._delta_buf: List[str] = []
        self._thinking_buf: List[str] = []
        self._delta_last_flush: float = 0.0
        self._mode: str = "edit"
        self._perm_overrides: Dict[str, str] = {}

    def set_window(self, window: Any) -> None:
        self._window = window
        self._ready.set()

    # ── Init ──

    def _ensure_engine(self) -> None:
        if self._controller is not None:
            return
        config = self._load_config_obj()
        self._engine = LLMEngine(config)
        self._registry = ToolRegistry()
        register_engine_tools(self._registry, self._gui_ref or _DummyGui())

        # Initialize MCP servers
        self._mcp = None
        try:
            from ai_editor.mcp_client import McpManager, load_mcp_configs
            settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
            getter = (lambda k, d=None: settings.get(k, d)) if settings else None
            configs = load_mcp_configs(getter)
            if configs:
                self._mcp = McpManager()
                for cfg in configs:
                    ok = self._mcp.add_server(cfg)
                    if ok:
                        client = self._mcp._clients.get(cfg.id)
                        n = len(client.tools) if client else 0
                        print(f"[MCP] Connected: {cfg.id} ({n} tools)")
        except Exception as exc:
            print(f"[MCP] Init failed: {exc}")

        sp = config.system_prompt or self._default_system_prompt()
        conv = Conversation(system_prompt=sp)
        self._controller = ChatController(self._engine, self._registry, conv)

        self._controller.on_stream_delta = self._on_stream_delta
        self._controller.on_thinking_delta = self._on_thinking_delta
        self._controller.on_stream_end = self._on_stream_end
        self._controller.on_tool_start = self._on_tool_start
        self._controller.on_tool_end = self._on_tool_end
        self._controller.on_tool_confirm = self._on_tool_confirm
        self._controller.on_tool_progress = self._on_tool_progress
        self._controller.on_token_warning = self._on_token_warning
        self._controller.on_error = self._on_error
        self._controller.on_idle = self._on_idle

        # Inject MCP tools into controller
        if self._mcp:
            self._controller.extra_tools = self._mcp.to_openai_tools()
            self._controller.mcp_dispatch = lambda name, args: self._mcp.call_tool(
                name, json.loads(args) if isinstance(args, str) else args
            )
            self._controller.mcp_tool_requires_confirm = self._mcp_tool_requires_confirm
            self._controller.mcp_tool_allowed = self._mcp_tool_allowed

        # @-mention variable resolver
        self._controller.resolve_variable = self._resolve_variable

        self._pending_confirm: Dict[str, threading.Event] = {}
        self._confirm_results: Dict[str, bool] = {}

        # Load mode from settings
        ai_cfg = _normalize_ai_editor_config(
            self._settings_getter("ai_editor", {}) or {})
        if isinstance(ai_cfg, dict):
            self._mode = ai_cfg.get("mode", "edit")
            self._perm_overrides = ai_cfg.get("permissions", {})

        # Chat provider registry + per-provider controllers
        from ai_editor.chat_providers import get_provider_registry
        self._provider_registry = get_provider_registry()
        self._provider_controllers: Dict[str, ChatController] = {}
        self._active_provider = "chat"

        # Extension host — lazy init in background thread
        from ai_editor.extension_host import get_extension_host
        self._ext_host = get_extension_host()
        self._extensions_inited = False
        threading.Thread(target=self.init_extensions, daemon=True).start()

        # VSCode API namespace
        from ai_editor.vscode_api import VscodeNamespace
        self._vscode_ns = VscodeNamespace(
            self._ext_host, self._engine, self._settings_getter)

        # Claude proxy (lazy — started on first CC provider use)
        self._claude_proxy = None

        # Agent & Workflow registries
        from ai_editor.agents import get_agent_registry
        from ai_editor.workflows import get_workflow_registry, WorkflowEngine
        self._agent_registry = get_agent_registry()
        self._wf_registry = get_workflow_registry()
        self._wf_engine = WorkflowEngine(self._engine, self._agent_registry)
        self._active_agent_id: Optional[str] = None

        gui = self._gui_ref or _DummyGui()
        actions = getattr(gui, '_ai_engine_actions', None)
        if not isinstance(actions, dict):
            actions = {}
            gui._ai_engine_actions = actions
        actions["list_agents"] = lambda **kw: self._eng_list_agents()
        actions["invoke_agent"] = lambda **kw: self._eng_invoke_agent(kw)
        actions["list_workflows"] = lambda **kw: self._eng_list_workflows()
        actions["run_workflow"] = lambda **kw: self._eng_run_workflow(kw)

    def _settings_getter(self, key: str, default=None):
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        if settings:
            return settings.get(key, default)
        return default

    def _default_system_prompt(self, agent_mode: bool = False) -> str:
        from ai_editor.prompts import get_system_prompt
        return get_system_prompt(agent_mode=agent_mode,
                                 settings_getter=self._settings_getter)

    # ── Config ──

    def _load_config_obj(self) -> ProviderConfig:
        return load_provider_config(self._gui_ref)

    # ── JS-callable methods (window.pywebview.api.*) ──

    def load_config(self) -> Dict:
        cfg = self._load_config_obj()
        # Load theme from ACT panel_themes or ai_editor config
        theme = "dark"
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        ai_cfg = {}
        if settings:
            ai_cfg = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
            theme = ai_cfg.get("theme", "")
            if not theme:
                themes = settings.get("panel_themes", {}) or {}
                theme = themes.get("act", "dark")
        pkeys = ai_cfg.get("provider_keys", {}) if isinstance(ai_cfg, dict) else {}
        model_name = cfg.model or cfg.effective_model
        ctx = cfg.effective_context
        result = {
            "provider": cfg.provider, "api_key": cfg.api_key,
            "base_url": cfg.base_url, "model": model_name,
            "temperature": cfg.temperature, "max_tokens": cfg.max_tokens,
            "transport": cfg.transport,
            "top_p": cfg.top_p,
            "frequency_penalty": cfg.frequency_penalty,
            "presence_penalty": cfg.presence_penalty,
            "stop": cfg.stop,
            "max_input_tokens": cfg.max_input_tokens,
            "max_output_tokens": cfg.max_output_tokens,
            "timeout": cfg.timeout,
            "extra_headers": cfg.extra_headers,
            "extra_body": cfg.extra_body,
            "system_prompt": cfg.system_prompt,
            "theme": theme,
            "_provider_keys": pkeys,
            "provider_keys": pkeys,
            "custom_models": ai_cfg.get("custom_models", {}),
            "mode": ai_cfg.get("mode", self._mode),
            "permissions": ai_cfg.get("permissions", self._perm_overrides),
            "context_window": ctx,
        }
        for section in _AI_EDITOR_SECTION_DEFAULTS:
            if isinstance(ai_cfg, dict) and section in ai_cfg:
                result[section] = ai_cfg[section]
        return result

    def save_config(self, data: Dict) -> Dict:
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        merged = _merge_ai_editor_config({}, data)
        if settings:
            merged = _merge_ai_editor_config(settings.get("ai_editor", {}) or {}, data)
            settings.set("ai_editor", merged)
            try:
                settings.save()
            except Exception:
                pass
        if self._engine:
            for k, v in merged.items():
                if k in _PROVIDER_CONFIG_KEYS and hasattr(self._engine.config, k):
                    setattr(self._engine.config, k, v)
        self._invalidate_provider_controllers(data)
        if isinstance(merged.get("mode"), str):
            self._mode = merged.get("mode") or self._mode
        if isinstance(merged.get("permissions"), dict):
            self._perm_overrides = dict(merged.get("permissions") or {})
        self._apply_mode_permissions()
        return {"ok": True}

    def _invalidate_provider_controllers(self, data: Dict) -> None:
        if not hasattr(self, "_provider_controllers"):
            return
        provider_keys = set(_PROVIDER_CONFIG_KEYS) | {"provider_keys", "claude_code", "codex"}
        if not any(k in data for k in provider_keys):
            return
        for provider_id in ("claude-code", "codex"):
            ctrl = self._provider_controllers.pop(provider_id, None)
            if ctrl:
                try:
                    ctrl.cancel()
                except Exception:
                    pass

    def send_message(self, text: str, config: Optional[Dict] = None, agent_mode: bool = False) -> Dict:
        if not text or not text.strip():
            return {"error": "Empty message"}
        self._ensure_engine()
        if config:
            if config.get("provider"):
                self._engine.config.provider = config["provider"]
            if config.get("model"):
                self._engine.config.model = config["model"]
        # @agent-id prefix → activate agent for this message
        stripped = text.strip()
        if stripped.startswith("@") and " " in stripped:
            mention, rest = stripped.split(" ", 1)
            agent_id = mention[1:]
            if self._agent_registry and self._agent_registry.get(agent_id):
                self._set_active_agent(agent_id)
                stripped = rest.strip()
        effective_agent = agent_mode or self._mode == "agent"
        if effective_agent and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt(agent_mode=True)
        self._apply_mode_permissions()
        self._controller.send(stripped, agent_mode=effective_agent)
        return {"ok": True}

    def _resolve_variable(self, name: str) -> str:
        """Resolve @-mention variables by reading editor state."""
        if name == "selection":
            r = self.editor_get_selection()
            return r.get("selection", "")
        if name == "editor":
            r = self.editor_get_content()
            return r.get("content", "")
        if name == "file":
            if self._window:
                try:
                    return self._window.evaluate_js("editorFileName") or "untitled"
                except Exception:
                    pass
            return "untitled"
        if name == "language":
            r = self.editor_get_language()
            return r.get("language", "plaintext")
        if name == "state":
            r = self.execute_tool("get_game_state", "{}")
            return r
        return ""

    def cancel(self) -> Dict:
        if self._controller:
            self._controller.cancel()
        return {"ok": True}

    # ── Window chrome (frameless) ──

    def win_minimize(self) -> None:
        if self._window:
            self._window.minimize()

    def win_maximize(self) -> None:
        if self._window:
            if getattr(self, '_maximized', False):
                self._window.restore()
                self._maximized = False
            else:
                self._window.maximize()
                self._maximized = True

    def win_close(self) -> None:
        if self._window:
            self._window.destroy()

    def new_chat(self) -> Dict:
        if self._controller:
            sp = self._engine.config.system_prompt if self._engine else ""
            self._controller.new_conversation(sp or self._default_system_prompt())
        return {"ok": True}

    # ── Custom Instructions API ──

    def get_instructions(self) -> Dict:
        """Return user instructions text + project instruction files."""
        from ai_editor.prompts import load_instructions, list_instruction_files
        ai = self._settings_getter("ai_editor", {}) or {}
        user_text = ai.get("user_instructions", "") if isinstance(ai, dict) else ""
        files = list_instruction_files()
        combined = load_instructions(settings_getter=self._settings_getter)
        return {"user_instructions": user_text, "files": files,
                "combined_preview": combined}

    def save_user_instructions(self, text: str) -> Dict:
        """Persist user-level instructions to settings."""
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        if not settings:
            return {"error": "Settings not available"}
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["user_instructions"] = text
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception:
            pass
        if self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return {"ok": True}

    def get_instruction_files(self) -> Dict:
        """List .sao/instructions.md and .sao/instructions/*.md files."""
        from ai_editor.prompts import list_instruction_files
        return {"files": list_instruction_files()}

    def save_instruction_file(self, name: str, content: str) -> Dict:
        """Create or update an instruction file under .sao/."""
        from ai_editor.prompts import save_instruction_file as _save
        result = _save(name, content)
        if result.get("ok") and self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return result

    def delete_instruction_file(self, name: str) -> Dict:
        """Delete an instruction file."""
        from ai_editor.prompts import delete_instruction_file as _del
        result = _del(name)
        if result.get("ok") and self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return result

    # ── Mode & Permission API ──

    def get_mode(self, mode: str = "") -> Dict:
        from ai_editor.scopes import MODES, MODE_PERMISSIONS, effective_permissions
        selected = mode if mode in MODES else self._mode
        tool_names = set(effective_permissions(selected, {}).keys())
        if self._registry:
            tool_names.update(t.name for t in self._registry.list_tools(include_disabled=True))
        return {
            "mode": self._mode,
            "selected_mode": selected,
            "modes": list(MODES),
            "permissions": effective_permissions(selected, self._perm_overrides),
            "defaults": dict(MODE_PERMISSIONS.get(selected, MODE_PERMISSIONS["edit"])),
            "overrides": dict(self._perm_overrides),
            "tools": sorted(tool_names),
        }

    def set_mode(self, mode: str) -> Dict:
        from ai_editor.scopes import MODES
        if mode not in MODES:
            return {"error": f"Invalid mode: {mode}. Valid: {list(MODES)}"}
        self._mode = mode
        self._save_mode_to_settings()
        self._apply_mode_permissions()
        return {"ok": True, "mode": mode}

    def set_tool_permission(self, tool_name: str, permission: str) -> Dict:
        if permission in ("", "default"):
            self._perm_overrides.pop(tool_name, None)
            self._save_mode_to_settings()
            self._apply_mode_permissions()
            return {"ok": True}
        if permission not in ("allowed", "confirm", "disabled"):
            return {"error": "Invalid permission"}
        self._perm_overrides[tool_name] = permission
        self._save_mode_to_settings()
        self._apply_mode_permissions()
        return {"ok": True}

    def get_scopes(self) -> Dict:
        from ai_editor.scopes import resolve_scopes
        scopes = resolve_scopes()
        for s in scopes:
            s["exists"] = os.path.isdir(s["path"])
        return {"scopes": scopes}

    def _save_mode_to_settings(self) -> None:
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        if not settings:
            return
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["mode"] = self._mode
        ai["permissions"] = self._perm_overrides
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception:
            pass

    def _apply_mode_permissions(self) -> None:
        """Update tool registry confirm flags + controller tool filter based on mode."""
        if not self._registry:
            return
        from ai_editor.scopes import effective_permissions
        perms = effective_permissions(self._mode, self._perm_overrides)
        disabled: List[str] = []
        for tool in self._registry.list_tools(include_disabled=True):
            p = perms.get(tool.name, "allowed")
            if p == "disabled":
                disabled.append(tool.name)
                tool.enabled = False
            elif p == "confirm":
                tool.enabled = True
                tool.requires_confirm = True
            else:
                tool.enabled = True
                tool.requires_confirm = False
        self._registry._openai_cache = None
        if self._controller:
            self._controller._disabled_tools = set(disabled)
            self._controller.mcp_tool_requires_confirm = self._mcp_tool_requires_confirm
            self._controller.mcp_tool_allowed = self._mcp_tool_allowed

    # ── Chat Provider API ──

    def list_chat_providers(self) -> Dict:
        self._ensure_engine()
        return {"providers": self._provider_registry.list_available(
            self._settings_getter)}

    def switch_provider(self, provider_id: str) -> Dict:
        self._ensure_engine()
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        self._active_provider = provider_id
        if provider_id == "chat":
            return {"ok": True, "provider": "chat"}
        ctrl = self._provider_controllers.get(provider_id)
        if not ctrl:
            ctrl = self._create_provider_controller(prov)
            self._provider_controllers[provider_id] = ctrl
        return {"ok": True, "provider": provider_id}

    def provider_send(self, provider_id: str, text: str) -> Dict:
        """Send a message to a specific provider's conversation."""
        self._ensure_engine()
        if provider_id == "chat":
            return self.send_message(text)
        prov = self._provider_registry.get(provider_id)
        if not prov:
            return {"error": f"Unknown provider: {provider_id}"}
        unsupported = self._unsupported_provider_transport(prov)
        if unsupported:
            return {"error": unsupported}
        ctrl = self._provider_controllers.get(provider_id)
        if not ctrl:
            ctrl = self._create_provider_controller(prov)
            self._provider_controllers[provider_id] = ctrl
        if ctrl.is_running:
            return {"error": "Already running"}
        ctrl.send(text.strip(), agent_mode=prov.auto_agent)
        return {"ok": True}

    def provider_cancel(self, provider_id: str) -> Dict:
        if provider_id == "chat":
            return self.cancel()
        ctrl = self._provider_controllers.get(provider_id)
        if ctrl:
            ctrl.cancel()
        return {"ok": True}

    def provider_new_chat(self, provider_id: str) -> Dict:
        if provider_id == "chat":
            return self.new_chat()
        ctrl = self._provider_controllers.get(provider_id)
        if ctrl:
            prov = self._provider_registry.get(provider_id)
            sp = prov.system_prompt if prov else ""
            ctrl.new_conversation(sp)
        return {"ok": True}

    def register_chat_provider(self, data: Dict) -> Dict:
        """Plugin API: register a custom chat provider tab."""
        self._ensure_engine()
        from ai_editor.chat_providers import ChatProviderDef
        prov = ChatProviderDef.from_dict(data)
        self._provider_registry.register(prov)
        return {"ok": True, "id": prov.id}

    def unregister_chat_provider(self, provider_id: str) -> Dict:
        self._ensure_engine()
        ctrl = self._provider_controllers.pop(provider_id, None)
        if ctrl:
            ctrl.cancel()
        self._provider_registry.unregister(provider_id)
        return {"ok": True}

    def _create_provider_controller(self, prov) -> ChatController:
        """Create a ChatController for a non-default provider."""
        from ai_editor.llm_engine import ProviderConfig
        key = self._resolve_provider_key(prov.provider_type)
        provider_settings = self._provider_section_settings(prov.id)
        base_cfg = self._engine.config if self._engine else ProviderConfig()
        transport = "chat_completions"
        if prov.id == "codex":
            transport = _normalize_transport(
                provider_settings.get("transport"), "chat_completions", _ENGINE_TRANSPORT_VALUES)
        cfg = ProviderConfig(
            provider=prov.provider_type,
            api_key=key or self._engine.config.api_key,
            base_url=prov.base_url or "",
            model=str(provider_settings.get("model") or prov.model),
            temperature=base_cfg.temperature,
            max_tokens=base_cfg.max_tokens,
            system_prompt=prov.system_prompt,
            transport=transport,
            top_p=base_cfg.top_p,
            frequency_penalty=base_cfg.frequency_penalty,
            presence_penalty=base_cfg.presence_penalty,
            stop=list(base_cfg.stop),
            max_input_tokens=base_cfg.max_input_tokens,
            max_output_tokens=base_cfg.max_output_tokens,
            timeout=base_cfg.timeout,
            extra_headers=dict(base_cfg.extra_headers),
            extra_body=dict(base_cfg.extra_body),
        )
        engine = LLMEngine(cfg)
        conv = Conversation(system_prompt=prov.system_prompt)
        ctrl = ChatController(engine, self._registry, conv)
        pid = prov.id
        ctrl.on_stream_delta = lambda msg, t: self._emit(
            f"provider_stream_delta", {"provider": pid, "content": t})
        ctrl.on_thinking_delta = lambda msg, t: self._emit(
            f"provider_thinking_delta", {"provider": pid, "content": t})
        ctrl.on_stream_end = lambda msg: self._emit(
            f"provider_stream_end", {"provider": pid,
             "content": msg.content, "model": msg.model,
             "thinking": msg.thinking,
             **({"error": msg.content} if msg.is_error else {}),
             **({"usage": msg.usage} if msg.usage else {})})
        ctrl.on_tool_start = lambda cid, n, a: self._emit(
            f"provider_tool_start", {"provider": pid, "id": cid, "name": n, "arguments": a})
        ctrl.on_tool_end = lambda cid, r: self._emit(
            f"provider_tool_end", {"provider": pid, "id": cid, "result": r})
        ctrl.on_error = lambda e: self._emit(
            f"provider_error", {"provider": pid, "error": e})
        ctrl.on_idle = lambda: self._emit(
            f"provider_idle", {"provider": pid})
        ctrl.resolve_variable = self._resolve_variable
        ctrl.mcp_tool_requires_confirm = self._mcp_tool_requires_confirm
        ctrl.mcp_tool_allowed = self._mcp_tool_allowed
        return ctrl

    def _provider_section_settings(self, provider_id: str) -> Dict[str, Any]:
        ai = _normalize_ai_editor_config(self._settings_getter("ai_editor", {}) or {})
        if provider_id == "claude-code":
            return _as_dict(ai.get("claude_code"))
        if provider_id == "codex":
            return _as_dict(ai.get("codex"))
        return {}

    def _unsupported_provider_transport(self, prov) -> str:
        settings = self._provider_section_settings(prov.id)
        if prov.id == "codex":
            transport = _normalize_transport(
                settings.get("transport"), "chat_completions", _CODEX_TRANSPORT_VALUES)
            if transport == "cli":
                return "Codex CLI transport is configured, but chat execution currently supports Chat Completions or Responses only."
        if prov.id == "claude-code" and settings.get("prefer_cli") is True:
            return "Claude Code CLI preference is configured, but chat execution currently uses the Anthropic API/proxy path."
        return ""

    def _resolve_provider_key(self, provider_type: str) -> str:
        ai = self._settings_getter("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            return ""
        if ai.get("provider") == provider_type:
            return ai.get("api_key", "")
        keys = ai.get("provider_keys", {})
        if isinstance(keys, dict):
            return keys.get(provider_type, "")
        return ""

    # ── Agent API (JS-callable) ──

    def list_agents(self) -> Dict:
        self._ensure_engine()
        agents = []
        for a in self._agent_registry.list_all():
            item = a.to_dict()
            if not getattr(a, "builtin", False):
                item["_scope"] = getattr(a, "_scope", "workspace")
                item["_plugin_id"] = getattr(a, "_plugin_id", "")
            agents.append(item)
        return {"agents": agents}

    def get_agent(self, agent_id: str) -> Dict:
        self._ensure_engine()
        a = self._agent_registry.get(agent_id)
        return a.to_dict() if a else {"error": "Not found"}

    def save_agent(self, data: Dict) -> Dict:
        self._ensure_engine()
        from ai_editor.agents import AgentDef
        scope = data.pop("_scope", "workspace")
        agent = AgentDef.from_dict(data)
        return self._agent_registry.save_custom(agent, scope=scope)

    def delete_agent(self, agent_id: str) -> Dict:
        self._ensure_engine()
        return self._agent_registry.delete_custom(agent_id)

    def set_active_agent(self, agent_id: str) -> Dict:
        self._ensure_engine()
        return self._set_active_agent(agent_id)

    def clear_active_agent(self) -> Dict:
        self._ensure_engine()
        self._active_agent_id = None
        if self._controller and self._controller.conversation:
            self._controller.conversation.system_prompt = self._default_system_prompt()
        return {"ok": True}

    def get_active_agent(self) -> Dict:
        return {"agent_id": self._active_agent_id or ""}

    def _set_active_agent(self, agent_id: str) -> Dict:
        agent = self._agent_registry.get(agent_id)
        if not agent:
            return {"error": f"Agent not found: {agent_id}"}
        self._active_agent_id = agent_id
        if self._controller and self._controller.conversation:
            base = self._default_system_prompt()
            self._controller.conversation.system_prompt = (
                base + f"\n\n# Active Agent: {agent.name}\n\n"
                + agent.system_prompt
            )
        return {"ok": True, "agent": agent.to_dict()}

    # ── Workflow API (JS-callable) ──

    def list_workflows(self) -> Dict:
        self._ensure_engine()
        workflows = []
        for w in self._wf_registry.list_all():
            item = w.to_dict()
            if not getattr(w, "builtin", False):
                item["_scope"] = getattr(w, "_scope", "workspace")
                item["_plugin_id"] = getattr(w, "_plugin_id", "")
            workflows.append(item)
        return {"workflows": workflows}

    def get_workflow(self, wf_id: str) -> Dict:
        self._ensure_engine()
        w = self._wf_registry.get(wf_id)
        return w.to_dict() if w else {"error": "Not found"}

    def save_workflow(self, data: Dict) -> Dict:
        self._ensure_engine()
        from ai_editor.workflows import WorkflowDef
        scope = data.pop("_scope", "workspace")
        wf = WorkflowDef.from_dict(data)
        return self._wf_registry.save_custom(wf, scope=scope)

    def delete_workflow(self, wf_id: str) -> Dict:
        self._ensure_engine()
        return self._wf_registry.delete_custom(wf_id)

    def run_workflow(self, wf_id: str, input_text: str) -> Dict:
        """Run a workflow from the UI. Executes in the calling thread."""
        self._ensure_engine()
        wf = self._wf_registry.get(wf_id)
        if not wf:
            return {"error": f"Workflow not found: {wf_id}"}

        def _on_start(i, total, step):
            self._emit("workflow_step", {
                "step": i, "total": total,
                "label": step.label, "status": "running"})

        def _on_end(i, total, step, output, error):
            self._emit("workflow_step", {
                "step": i, "total": total,
                "label": step.label, "status": "done",
                "preview": (output or "")[:300], "error": error})

        return self._wf_engine.run(wf, input_text, _on_start, _on_end)

    # ── Engine action handlers (registered on gui._ai_engine_actions) ──

    def _eng_list_agents(self) -> Dict:
        return {"agents": [
            {"id": a.id, "name": a.name, "description": a.description,
             "icon": a.icon, "when_to_use": a.when_to_use}
            for a in self._agent_registry.list_all()
        ]}

    def _eng_invoke_agent(self, kw: Dict) -> Dict:
        agent_id = kw.get("agent_id", "")
        message = kw.get("message", "")
        agent = self._agent_registry.get(agent_id)
        if not agent:
            return {"error": f"Agent not found: {agent_id}",
                    "available": [a.id for a in self._agent_registry.list_all()]}
        self._set_active_agent(agent_id)
        return {
            "agent": agent.id,
            "name": agent.name,
            "activated": True,
            "instruction": (
                f"Now acting as {agent.name}. "
                f"Apply this guidance:\n\n{agent.system_prompt}"
            ),
        }

    def _eng_list_workflows(self) -> Dict:
        return {"workflows": [
            {"id": w.id, "name": w.name, "description": w.description,
             "icon": w.icon, "steps": len(w.steps),
             "when_to_use": w.when_to_use}
            for w in self._wf_registry.list_all()
        ]}

    def _eng_run_workflow(self, kw: Dict) -> Dict:
        wf_id = kw.get("workflow_id", "")
        input_text = kw.get("input", "")
        wf = self._wf_registry.get(wf_id)
        if not wf:
            return {"error": f"Workflow not found: {wf_id}",
                    "available": [w.id for w in self._wf_registry.list_all()]}
        return self._wf_engine.run(wf, input_text)

    # ── Extension Host API ──

    def init_extensions(self) -> None:
        """Scan and activate extensions. Safe to call from a background thread."""
        if self._extensions_inited:
            return
        self._extensions_inited = True
        self._init_extension_host()

    def _init_extension_host(self) -> None:
        """Scan extension directories and start the host."""
        try:
            from ai_editor.scopes import _base_dir
            base = _base_dir()
        except Exception:
            base = os.path.dirname(os.path.dirname(__file__))
        ext_dirs = []
        ai_ext = os.path.join(base, "ai_editor_extensions")
        if os.path.isdir(ai_ext):
            ext_dirs.append(ai_ext)
        home_ext = os.path.join(os.path.expanduser("~"), ".sao", "extensions")
        if os.path.isdir(home_ext):
            ext_dirs.append(home_ext)
        # VSCode extensions directory
        vscode_ext = os.path.join(os.path.expanduser("~"),
                                   ".vscode", "extensions")
        if os.path.isdir(vscode_ext):
            ext_dirs.append(vscode_ext)
        vscode_insiders_ext = os.path.join(
            os.path.expanduser("~"), ".vscode-insiders", "extensions")
        if os.path.isdir(vscode_insiders_ext):
            ext_dirs.append(vscode_insiders_ext)
        self._ext_host.set_policy(
            self._extension_allowed,
            set(self._enabled_extension_contributions()),
        )
        count = self._ext_host.scan(ext_dirs)
        if count:
            activated = self._ext_host.start()
            print(f"[ExtHost] {count} extensions scanned, "
                  f"{len(activated)} activated")
            self._register_ext_tools()

    def _register_ext_tools(self) -> None:
        """Register extension-contributed tools and chat participants."""
        ep = self._ext_host.ext_points
        enabled = set(self._enabled_extension_contributions())
        for tool in ep.language_model_tools:
            if "languageModelTools" not in enabled:
                continue
            name = tool.get("name", "")
            if not name:
                continue
            schema = tool.get("inputSchema") or tool.get("parametersSchema") or {
                "type": "object", "properties": {}}
            self._registry.register(
                name=f"ext_{name}",
                description=tool.get("displayName", name),
                parameters=schema,
                handler=lambda _n=name, **kw: {"stub": True, "tool": _n, **kw},
                category=f"ext:{tool.get('_extensionId', '')}",
            )
        for cp in ep.chat_participants:
            if "chatParticipants" not in enabled:
                continue
            pid = cp.get("id") or cp.get("name", "")
            if not pid:
                continue
            from ai_editor.chat_providers import ChatProviderDef
            prov = ChatProviderDef(
                id=f"ext-{pid}",
                name=cp.get("fullName") or cp.get("name", pid),
                icon=cp.get("icon", "\U0001f916"),
                provider_type=cp.get("_provider_type", "openai"),
                system_prompt=cp.get("description", ""),
                auto_agent=True,
            )
            self._provider_registry.register(prov)

    def _extension_settings(self) -> Dict[str, Any]:
        ai = _normalize_ai_editor_config(self._settings_getter("ai_editor", {}) or {})
        return _as_dict(ai.get("extensions"))

    def _enabled_extension_contributions(self) -> List[str]:
        ext = self._extension_settings()
        raw = ext.get("enabled_contributions", [])
        if isinstance(raw, list) and raw:
            return [str(x) for x in raw]
        return list(_AI_EDITOR_SECTION_DEFAULTS["extensions"]["enabled_contributions"])

    @staticmethod
    def _publisher_from_extension_id(ext_id: str) -> str:
        return ext_id.split(".", 1)[0].strip().lower() if "." in ext_id else ""

    def _extension_allowed(self, ext: Any) -> bool:
        settings = self._extension_settings()
        publisher = str(getattr(ext, "publisher", "") or "").strip().lower()
        if not publisher:
            publisher = self._publisher_from_extension_id(str(getattr(ext, "id", "")))
        blocked = {str(x).strip().lower() for x in settings.get("blocked_publishers", []) if str(x).strip()}
        allowed = {str(x).strip().lower() for x in settings.get("allowed_publishers", []) if str(x).strip()}
        if publisher in blocked:
            return False
        if allowed and publisher not in allowed:
            return False
        return True

    def _extension_id_allowed(self, ext_id: str) -> bool:
        class _Ext:
            pass
        ext = _Ext()
        ext.id = ext_id
        ext.publisher = self._publisher_from_extension_id(ext_id)
        return self._extension_allowed(ext)

    def list_vscode_extensions(self) -> Dict:
        self._ensure_engine()
        return {"extensions": self._ext_host.list_extensions(),
                "contributes": self._ext_host.get_contributes_summary()}

    def activate_extension(self, ext_id: str) -> Dict:
        self._ensure_engine()
        ext = self._ext_host.registry.get(ext_id)
        if ext and not self._extension_allowed(ext):
            return {"error": f"Extension blocked by trust policy: {ext_id}"}
        act = self._ext_host.activator.activate(ext_id)
        if act:
            self._register_ext_tools()
            return {"ok": True, "id": ext_id,
                    "activationTimeMs": act.activation_time_ms}
        return {"error": f"Failed to activate: {ext_id}"}

    def execute_command(self, command_id: str, *args: Any) -> Dict:
        self._ensure_engine()
        try:
            result = self._ext_host.commands.execute(command_id, *args)
            if isinstance(result, dict):
                return result
            return {"result": result}
        except KeyError:
            return {"error": f"Command not found: {command_id}"}
        except Exception as exc:
            return {"error": str(exc)}

    def list_commands(self) -> Dict:
        self._ensure_engine()
        return {"commands": self._ext_host.commands.list_commands()}

    # ── VSCode API ──

    def get_vscode_api(self) -> Dict:
        """Return summary of the vscode.* namespace state."""
        self._ensure_engine()
        return {
            "chat_participants": list(self._vscode_ns.chat_participants.keys()),
            "lm_tools": list(self._vscode_ns.registered_tools.keys()),
            "variables": list(self._vscode_ns.variables.keys()),
            "commands": self._ext_host.commands.list_commands(),
        }

    def invoke_chat_participant(self, participant_id: str,
                                 prompt: str) -> Dict:
        """Invoke a registered chat participant's handler."""
        self._ensure_engine()
        from ai_editor.vscode_api import (
            ChatRequest, ChatContext, ChatResponseStream)
        cp = self._vscode_ns.chat_participants.get(participant_id)
        if not cp:
            return {"error": f"Participant not found: {participant_id}"}
        req = ChatRequest(prompt=prompt)
        ctx = ChatContext()
        parts = []
        stream = ChatResponseStream(lambda kind, val: parts.append(val))
        try:
            result = cp.request_handler(req, ctx, stream, None)
            return {"ok": True, "content": stream.get_content(),
                    "result": str(result) if result else None}
        except Exception as exc:
            return {"error": str(exc)}

    def invoke_lm_tool(self, tool_name: str, input_data: Any = None) -> Dict:
        """Invoke a registered LM tool."""
        self._ensure_engine()
        from ai_editor.vscode_api import LanguageModelToolInvocationOptions
        tool = self._vscode_ns.registered_tools.get(tool_name)
        if not tool:
            return {"error": f"Tool not found: {tool_name}"}
        opts = LanguageModelToolInvocationOptions(input=input_data)
        try:
            result = tool.invoke(opts, None)
            if hasattr(result, "content"):
                return {"ok": True, "content": result.content}
            return {"ok": True, "result": str(result)}
        except Exception as exc:
            return {"error": str(exc)}

    # ── Authentication ──

    def list_auth_sessions(self, provider_id: str = "") -> Dict:
        from ai_editor.auth import get_auth_service
        sessions = get_auth_service().list_sessions(provider_id)
        return {"sessions": [s.to_dict() for s in sessions]}

    def create_auth_session(self, provider_id: str, token: str,
                             label: str = "") -> Dict:
        from ai_editor.auth import get_auth_service
        session = get_auth_service().create_session_from_token(
            provider_id, token, label)
        return {"ok": True, "session": session.to_dict()}

    def remove_auth_session(self, provider_id: str,
                             session_id: str) -> Dict:
        from ai_editor.auth import get_auth_service
        ok = get_auth_service().remove_session(provider_id, session_id)
        return {"ok": ok}

    # ── Claude Proxy ──

    def start_claude_proxy(self) -> Dict:
        """Start the local Anthropic-compatible proxy for Claude Code SDK."""
        self._ensure_engine()
        if self._claude_proxy and self._claude_proxy.is_running:
            return {"ok": True, "port": self._claude_proxy.port,
                    "base_url": self._claude_proxy.base_url}
        from ai_editor.claude_proxy import ClaudeProxy
        self._claude_proxy = ClaudeProxy(self._engine)
        port = self._claude_proxy.start()
        return {"ok": True, "port": port,
                "base_url": self._claude_proxy.base_url,
                "env": self._claude_proxy.get_env()}

    def stop_claude_proxy(self) -> Dict:
        if self._claude_proxy:
            self._claude_proxy.stop()
        return {"ok": True}

    def get_claude_proxy_status(self) -> Dict:
        if self._claude_proxy and self._claude_proxy.is_running:
            return {"running": True, "port": self._claude_proxy.port,
                    "base_url": self._claude_proxy.base_url}
        return {"running": False}

    def install_extension_dir(self, ext_dir: str) -> Dict:
        self._ensure_engine()
        desc = self._ext_host.install_from_dir(ext_dir)
        if desc:
            if not self._extension_allowed(desc):
                return {"error": f"Extension blocked by trust policy: {desc.id}"}
            self._register_ext_tools()
            return {"ok": True, "id": desc.id, "name": desc.display_name}
        return {"error": "Failed to install from directory"}

    def export_chat(self) -> str:
        if not self._controller:
            return "[]"
        return self._controller.export_messages()

    def list_tools(self) -> Dict:
        self._ensure_engine()
        tools = [
            {"name": t.name, "description": t.description,
             "category": t.category, "requires_confirm": t.requires_confirm,
             "parameters": t.parameters}
            for t in self._registry.list_tools()
        ]
        # Add MCP tools
        if self._mcp:
            mcp_access = self._mcp_access()
            for t in self._mcp.all_tools():
                tool_name = f"mcp_{t.server_id}_{t.name}"
                if not self._mcp_tool_allowed(tool_name):
                    continue
                tools.append({
                    "name": tool_name,
                    "description": f"[MCP:{t.server_id}] {t.description}",
                    "category": f"mcp:{t.server_id}",
                    "requires_confirm": mcp_access == "prompt",
                    "parameters": t.input_schema,
                })
        # Add extension-contributed tools
        try:
            from ai_editor.extensions import load_all_extension_tools
            enabled = set(self._enabled_extension_contributions())
            for et in load_all_extension_tools(enabled):
                if not self._extension_id_allowed(et.get("extension_id", "")):
                    continue
                fn = et.get("function", {})
                tools.append({
                    "name": fn.get("name", ""),
                    "description": fn.get("description", ""),
                    "category": f"ext:{et.get('extension_id','')}",
                    "requires_confirm": False,
                    "parameters": fn.get("parameters", {}),
                })
        except Exception:
            pass
        return {"tools": tools}

    def execute_tool(self, name: str, arguments: str = "{}", confirmed: bool = False) -> str:
        self._ensure_engine()
        if name.startswith("mcp_") and self._mcp:
            if not self._mcp_tool_allowed(name):
                return json.dumps({"error": f"MCP tool disabled by policy: {name}"}, ensure_ascii=False)
            if self._mcp_tool_requires_confirm(name) and not confirmed:
                return json.dumps({"error": "Tool execution requires confirmation", "requires_confirmation": True}, ensure_ascii=False)
            args = json.loads(arguments) if isinstance(arguments, str) else arguments
            return self._mcp.call_tool(name, args)
        from ai_editor.scopes import tool_permission
        perm = tool_permission(self._mode, name, self._perm_overrides)
        if perm == "disabled":
            return json.dumps({"error": f"Tool disabled by mode policy: {name}"}, ensure_ascii=False)
        if perm == "confirm" and not confirmed:
            return json.dumps({"error": "Tool execution requires confirmation", "requires_confirmation": True}, ensure_ascii=False)
        return self._registry.execute(name, arguments)

    def _mcp_settings(self) -> Dict[str, Any]:
        ai = _normalize_ai_editor_config(self._settings_getter("ai_editor", {}) or {})
        return _as_dict(ai.get("mcp"))

    def _mcp_access(self) -> str:
        access = str(self._mcp_settings().get("access", "prompt")).strip().lower()
        return access if access in {"prompt", "read_only", "allow", "disabled"} else "prompt"

    @staticmethod
    def _is_probably_read_only_mcp_tool(name: str) -> bool:
        leaf = name.rsplit("_", 1)[-1].lower()
        return leaf.startswith(("read", "list", "get", "search", "show", "fetch", "query", "find"))

    def _mcp_tool_allowed(self, name: str) -> bool:
        access = self._mcp_access()
        if access == "disabled":
            return False
        if access == "read_only":
            return self._is_probably_read_only_mcp_tool(name)
        return True

    def _mcp_tool_requires_confirm(self, name: str) -> bool:
        return self._mcp_access() == "prompt" and self._mcp_tool_allowed(name)

    def _refresh_mcp_tools(self) -> None:
        if self._controller:
            self._controller.extra_tools = self._mcp.to_openai_tools() if self._mcp else []
            self._controller.mcp_tool_requires_confirm = self._mcp_tool_requires_confirm
            self._controller.mcp_tool_allowed = self._mcp_tool_allowed

    def list_mcp_servers(self) -> Dict:
        self._ensure_engine()
        saved = self._saved_mcp_servers()
        live = {s.get("id"): s for s in (self._mcp.list_servers() if self._mcp else [])}
        result = []
        for entry in saved:
            item = dict(entry)
            if item.get("id") in live:
                item.update(live[item.get("id")])
                item["configured"] = True
            else:
                item["running"] = False
                item["configured"] = True
            result.append(item)
        for sid, item in live.items():
            if not any(s.get("id") == sid for s in result):
                item = dict(item)
                item["configured"] = False
                result.append(item)
        return {"servers": result}

    def add_mcp_server(self, config: Dict) -> Dict:
        self._ensure_engine()
        if not self._mcp:
            from ai_editor.mcp_client import McpManager
            self._mcp = McpManager()
        from ai_editor.mcp_client import McpServerConfig
        server = {
            "id": str(config.get("id", "")).strip(),
            "name": str(config.get("name", config.get("id", ""))).strip(),
            "transport": str(config.get("transport", "stdio")),
            "command": str(config.get("command", "")),
            "args": list(config.get("args", [])) if isinstance(config.get("args", []), list) else [],
            "env": dict(config.get("env", {})) if isinstance(config.get("env", {}), dict) else {},
            "url": str(config.get("url", "")),
            "headers": dict(config.get("headers", {})) if isinstance(config.get("headers", {}), dict) else {},
            "enabled": bool(config.get("enabled", True)),
        }
        if not server["id"]:
            return {"error": "MCP server id is required"}
        self._upsert_saved_mcp_server(server)
        cfg = McpServerConfig(
            id=server["id"],
            name=server["name"] or server["id"],
            transport=server["transport"],
            command=server["command"],
            args=server["args"],
            env=server["env"],
            url=server["url"],
            headers=server["headers"],
            enabled=server["enabled"],
        )
        ok = self._mcp.add_server(cfg) if server["enabled"] and self._mcp_settings().get("autostart") else True
        tool_count = 0
        if ok:
            client = self._mcp._clients.get(cfg.id)
            if client:
                tool_count = len(client.tools)
        self._refresh_mcp_tools()
        return {"ok": ok, "id": cfg.id, "tools": tool_count}

    def remove_mcp_server(self, server_id: str) -> Dict:
        self._remove_saved_mcp_server(server_id)
        if self._mcp:
            self._mcp.remove_server(server_id)
        self._refresh_mcp_tools()
        return {"ok": True}

    def _saved_mcp_servers(self) -> List[Dict[str, Any]]:
        mcp = self._mcp_settings()
        raw = mcp.get("servers", [])
        if isinstance(raw, list):
            return [dict(s) for s in raw if isinstance(s, dict) and s.get("id")]
        if isinstance(raw, dict):
            return [dict(v, id=str(k)) for k, v in raw.items() if isinstance(v, dict)]
        return []

    def _save_mcp_servers(self, servers: List[Dict[str, Any]]) -> None:
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        if not settings:
            return
        ai = _normalize_ai_editor_config(settings.get("ai_editor", {}) or {})
        mcp = _as_dict(ai.get("mcp"))
        mcp["servers"] = servers
        ai["mcp"] = mcp
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception:
            pass

    def _upsert_saved_mcp_server(self, server: Dict[str, Any]) -> None:
        servers = [s for s in self._saved_mcp_servers() if s.get("id") != server.get("id")]
        servers.append(server)
        self._save_mcp_servers(servers)

    def _remove_saved_mcp_server(self, server_id: str) -> None:
        self._save_mcp_servers([s for s in self._saved_mcp_servers() if s.get("id") != server_id])

    def register_mcp_tools(self, server_id: str, tools: list, handlers: dict = None) -> Dict:
        """Register internal (Python-native) MCP tools — no subprocess needed.

        Called by plugins to expose their tools as MCP-compatible entries.
        tools: [{"name":"...", "description":"...", "inputSchema":{...}}]
        handlers: {"tool_name": callable}
        """
        self._ensure_engine()
        if not self._mcp:
            from ai_editor.mcp_client import McpManager
            self._mcp = McpManager()
        provider = self._mcp.register_internal(server_id, tools, handlers)
        if self._controller:
            self._controller.extra_tools = self._mcp.to_openai_tools()
        return {"ok": True, "id": server_id, "tools": len(provider.tools)}

    def test_connection(self, cfg: Dict) -> Dict:
        self._ensure_engine()
        test_cfg = ProviderConfig(
            provider=cfg.get("provider", "openai"),
            api_key=cfg.get("api_key", ""),
            base_url=cfg.get("base_url", ""),
            model=cfg.get("model", ""),
            transport=_normalize_transport(cfg.get("transport")),
        )
        ok, msg = self._engine.test_connection(test_cfg)
        return {"ok": ok, "message": msg}

    def list_history(self) -> Dict:
        try:
            from ai_editor.history import list_conversations
            return {"entries": list_conversations(limit=50)}
        except Exception as exc:
            return {"error": str(exc)}

    def load_history(self, conv_id: str) -> Dict:
        try:
            from ai_editor.history import load_conversation
            data = load_conversation(conv_id)
            return data or {"error": "Not found"}
        except Exception as exc:
            return {"error": str(exc)}

    def delete_history(self, conv_id: str) -> Dict:
        try:
            from ai_editor.history import delete_conversation
            return {"ok": delete_conversation(conv_id)}
        except Exception as exc:
            return {"error": str(exc)}

    # ── Extension marketplace API ──

    def search_extensions(self, query: str = "ai chat model", page: int = 1) -> Dict:
        """Search VSCode Marketplace. Returns list of extensions."""
        try:
            from ai_editor.extensions import search_extensions, is_installed
            results = search_extensions(query, page=page)
            for r in results:
                if isinstance(r, dict) and "id" in r:
                    r["installed"] = is_installed(r["id"])
            return {"extensions": results}
        except Exception as exc:
            return {"error": str(exc)}

    def install_extension(self, ext_id: str, vsix_url: str = "", confirmed: bool = False) -> Dict:
        """Install an extension from the marketplace."""
        try:
            policy = self._extension_settings()
            if policy.get("confirm_install", True) and not confirmed:
                return {"error": "Extension install requires confirmation", "requires_confirmation": True}
            if not self._extension_id_allowed(ext_id):
                return {"error": f"Extension blocked by trust policy: {ext_id}"}
            from ai_editor.extensions import install_extension
            return install_extension(ext_id, vsix_url)
        except Exception as exc:
            return {"error": str(exc)}

    def uninstall_extension(self, ext_id: str, confirmed: bool = False) -> Dict:
        """Uninstall an extension."""
        try:
            policy = self._extension_settings()
            if policy.get("confirm_install", True) and not confirmed:
                return {"error": "Extension uninstall requires confirmation", "requires_confirmation": True}
            from ai_editor.extensions import uninstall_extension
            return uninstall_extension(ext_id)
        except Exception as exc:
            return {"error": str(exc)}

    def list_installed_extensions(self) -> Dict:
        """List locally installed extensions."""
        try:
            from ai_editor.extensions import list_installed
            return {"extensions": list_installed()}
        except Exception as exc:
            return {"error": str(exc)}

    def get_extension_detail(self, publisher: str, name: str) -> Dict:
        """Fetch a single extension detail from marketplace."""
        try:
            from ai_editor.extensions import get_extension_detail, is_installed
            result = get_extension_detail(publisher, name)
            if result:
                result["installed"] = is_installed(result["id"])
                return result
            return {"error": "Not found"}
        except Exception as exc:
            return {"error": str(exc)}

    def get_model_info(self, model: str = "") -> Dict:
        """Return context window info for a model."""
        from ai_editor.llm_engine import get_model_context, compaction_threshold
        m = model or (self._engine.config.effective_model if self._engine else "")
        ctx = get_model_context(m)
        return {"model": m, "max_input": ctx["max_input"],
                "max_output": ctx["max_output"],
                "compact_at": compaction_threshold(m)}

    def list_models(self) -> Dict:
        """Return all known models (built-in + custom)."""
        from ai_editor.llm_engine import list_all_models
        return {"models": list_all_models()}

    def save_custom_model(self, model_name: str, max_input: int = 128000,
                          max_output: int = 4096, tools: bool = True,
                          vision: bool = False, thinking: bool = False,
                          streaming: bool = True) -> Dict:
        """Add or update a model definition."""
        from ai_editor.llm_engine import register_model
        register_model(model_name, max_input, max_output,
                       tools, vision, thinking, streaming)
        self._save_models_to_settings()
        return {"ok": True, "model": model_name}

    def delete_custom_model(self, model_name: str) -> Dict:
        from ai_editor.llm_engine import unregister_model
        unregister_model(model_name)
        self._save_models_to_settings()
        return {"ok": True}

    def _save_models_to_settings(self) -> None:
        from ai_editor.llm_engine import _model_registry
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        if not settings:
            return
        ai = settings.get("ai_editor", {}) or {}
        if not isinstance(ai, dict):
            ai = {}
        ai["custom_models"] = dict(_model_registry)
        settings.set("ai_editor", ai)
        try:
            settings.save()
        except Exception:
            pass

    def get_full_config(self) -> Dict:
        """Return ALL configurable parameters for the active endpoint."""
        self._ensure_engine()
        c = self._engine.config
        return {
            "provider": c.provider, "model": c.effective_model,
            "base_url": c.effective_base_url,
            "temperature": c.temperature, "top_p": c.top_p,
            "max_tokens": c.max_tokens,
            "frequency_penalty": c.frequency_penalty,
            "presence_penalty": c.presence_penalty,
            "stop": c.stop, "timeout": c.timeout,
            "max_input_tokens": c.max_input_tokens,
            "max_output_tokens": c.max_output_tokens,
            "extra_headers": c.extra_headers,
            "extra_body": c.extra_body,
            "context_window": c.effective_context,
        }

    def count_tokens(self, text: str = "") -> Dict:
        self._ensure_engine()
        count = self._engine.estimate_tokens(text)
        return {"tokens": count, "model": self._engine.config.effective_model}

    def count_conversation_tokens(self) -> Dict:
        self._ensure_engine()
        if not self._controller or not self._controller.conversation:
            return {"tokens": 0}
        msgs = self._controller.conversation.to_api_messages()
        count = self._engine.count_message_tokens(msgs)
        return {"tokens": count, "messages": len(msgs)}

    def send_image_message(self, text: str, image_base64: str, mime: str = "image/png") -> Dict:
        """Send a message with an attached image (vision)."""
        if not image_base64:
            return {"error": "No image data"}
        self._ensure_engine()
        if self._is_anthropic():
            content = self._engine.make_image_content_anthropic(text or "What is this image?", image_base64, mime)
        else:
            content = self._engine.make_image_content(text or "What is this image?", image_base64, mime)
        from ai_editor.chat_state import ChatMessage
        user_msg = ChatMessage(role="user", content=text or "(image)")
        self._controller.conversation.add_message(user_msg)
        # Directly call with multimodal content
        msgs = self._controller.conversation.to_api_messages()
        msgs[-1]["content"] = content
        self._controller._running = True
        import threading as _th
        def _run():
            try:
                tools = self._controller.registry.to_openai_tools() or None
                self._controller.engine.reset_cancel()
                resp = self._controller.engine.chat_completion_stream(
                    messages=msgs, tools=tools,
                    on_delta=lambda d: (d.content and self._emit("stream_delta", {"content": d.content})),
                )
                self._emit("stream_end", {
                    "content": resp.content, "model": resp.model,
                    "thinking": resp.thinking,
                    "usage": resp.usage,
                    **({"error": resp.error} if resp.error else {}),
                })
            finally:
                self._controller._running = False
                self._emit("idle", {})
        _th.Thread(target=_run, daemon=True).start()
        return {"ok": True}

    def _is_anthropic(self) -> bool:
        return self._engine and self._engine._is_anthropic_native(self._engine.config)

    def confirm_tool(self, call_id: str, allowed: bool) -> Dict:
        """UI calls this to allow/deny a pending tool confirmation."""
        evt = self._pending_confirm.pop(call_id, None)
        self._confirm_results[call_id] = allowed
        if evt:
            evt.set()
        return {"ok": True}

    # ── Editor state API (called by JS, also used by editor tools) ──

    def editor_get_content(self) -> Dict:
        """Get editor content + language in a single JS eval."""
        if not self._window:
            return {"content": "", "language": "plaintext"}
        try:
            raw = self._window.evaluate_js(
                "JSON.stringify({c:document.getElementById('editor-text').value,l:editorLang})"
            )
            d = json.loads(raw) if raw else {}
            return {"content": d.get("c", ""), "language": d.get("l", "plaintext")}
        except Exception:
            return {"content": "", "language": "plaintext"}

    def editor_set_content(self, content: str, language: str = "", filename: str = "") -> Dict:
        """Set editor content."""
        js = json.dumps(content)
        self._eval_js(f"openInEditor({js},{json.dumps(language or '')})")
        if filename:
            self._eval_js(f"editorFileName={json.dumps(filename)}")
        return {"ok": True, "length": len(content)}

    def editor_insert_text(self, text: str) -> Dict:
        """Insert text at cursor position."""
        js = json.dumps(text)
        self._eval_js(f"""(function(){{
            var ed=document.getElementById('editor-text');
            var s=ed.selectionStart;
            ed.value=ed.value.substring(0,s)+{js}+ed.value.substring(ed.selectionEnd);
            ed.selectionStart=ed.selectionEnd=s+{len(text)};
            updateLineNums();updateCursorPos();
        }})()""")
        return {"ok": True}

    def editor_get_selection(self) -> Dict:
        """Get selected text from editor."""
        if not self._window:
            return {"selection": "", "start": 0, "end": 0}
        try:
            result = self._window.evaluate_js("""
                (function(){
                    var ed=document.getElementById('editor-text');
                    return JSON.stringify({
                        selection:ed.value.substring(ed.selectionStart,ed.selectionEnd),
                        start:ed.selectionStart, end:ed.selectionEnd
                    });
                })()
            """)
            return json.loads(result) if result else {"selection": "", "start": 0, "end": 0}
        except Exception:
            return {"selection": "", "start": 0, "end": 0}

    def editor_go_to_line(self, line: int) -> Dict:
        """Navigate editor to a specific line."""
        self._eval_js(f"""(function(){{
            var ed=document.getElementById('editor-text');
            var lines=ed.value.split('\\n');
            var pos=0;for(var i=0;i<Math.min({line}-1,lines.length-1);i++)pos+=lines[i].length+1;
            ed.selectionStart=ed.selectionEnd=pos;ed.focus();
            updateCursorPos();ed.scrollTop=Math.max(0,({line}-10)*18);
        }})()""")
        return {"ok": True, "line": line}

    def editor_find_replace(self, find: str, replace: str, replace_all: bool = False) -> Dict:
        """Find and replace in editor."""
        f = json.dumps(find)
        r = json.dumps(replace)
        if replace_all:
            self._eval_js(f"""(function(){{
                var ed=document.getElementById('editor-text');
                ed.value=ed.value.split({f}).join({r});updateLineNums();
            }})()""")
        else:
            self._eval_js(f"""(function(){{
                var ed=document.getElementById('editor-text');
                var idx=ed.value.indexOf({f},ed.selectionEnd);
                if(idx===-1)idx=ed.value.indexOf({f});
                if(idx>=0){{
                    ed.value=ed.value.substring(0,idx)+{r}+ed.value.substring(idx+{len(find)});
                    ed.selectionStart=idx;ed.selectionEnd=idx+{len(replace)};
                    updateLineNums();
                }}
            }})()""")
        return {"ok": True}

    def editor_get_language(self) -> Dict:
        """Get current editor language mode."""
        if not self._window:
            return {"language": "plaintext"}
        try:
            lang = self._window.evaluate_js("editorLang")
            return {"language": lang or "plaintext"}
        except Exception:
            return {"language": "plaintext"}

    # ── Events pushed to JS ──

    def _eval_js(self, js: str) -> None:
        if self._window:
            try:
                self._window.evaluate_js(js)
            except Exception:
                pass

    def _emit(self, event: str, data: Any) -> None:
        payload = json.dumps(data, ensure_ascii=False, default=str)
        self._eval_js(f"window._onEditorEvent&&window._onEditorEvent({json.dumps(event)},{payload})")

    def _on_stream_delta(self, msg: ChatMessage, text: str) -> None:
        self._delta_buf.append(text)
        now = time.monotonic()
        if len(self._delta_buf) >= 15 or (now - self._delta_last_flush) > 0.05:
            self._flush_deltas()

    def _on_thinking_delta(self, msg: ChatMessage, text: str) -> None:
        self._thinking_buf.append(text)
        if len(self._thinking_buf) >= 10:
            self._flush_thinking()

    def _flush_deltas(self) -> None:
        if not self._delta_buf:
            return
        combined = "".join(self._delta_buf)
        self._delta_buf.clear()
        self._delta_last_flush = time.monotonic()
        self._emit("stream_delta", {"content": combined})

    def _flush_thinking(self) -> None:
        if not self._thinking_buf:
            return
        combined = "".join(self._thinking_buf)
        self._thinking_buf.clear()
        self._emit("thinking_delta", {"content": combined})

    def _on_stream_end(self, msg: ChatMessage) -> None:
        self._flush_deltas()
        self._flush_thinking()
        payload: Dict[str, Any] = {"content": msg.content, "model": msg.model}
        if msg.thinking:
            payload["thinking"] = msg.thinking
        if msg.tool_calls:
            payload["tool_calls"] = [
                {"id": tc.id, "name": tc.name, "arguments": tc.arguments}
                for tc in msg.tool_calls
            ]
        if msg.is_error:
            payload["error"] = msg.content
        if msg.usage:
            payload["usage"] = msg.usage
        self._emit("stream_end", payload)

    def _on_tool_start(self, call_id: str, name: str, args: str) -> None:
        self._emit("tool_start", {"id": call_id, "name": name, "arguments": args})

    def _on_tool_confirm(self, call_id: str, name: str, args: str) -> bool:
        """Called from background thread. Pushes confirm request to JS, blocks until response."""
        evt = threading.Event()
        self._pending_confirm[call_id] = evt
        self._confirm_results[call_id] = True
        self._emit("tool_confirm", {"id": call_id, "name": name, "arguments": args})
        evt.wait(timeout=60.0)
        return self._confirm_results.pop(call_id, True)

    def _on_tool_end(self, call_id: str, result: str, state: str = "") -> None:
        self._emit("tool_end", {"id": call_id, "result": result, "state": state})

    def _on_tool_progress(self, call_id: str, name: str, progress: float) -> None:
        self._emit("tool_progress", {"id": call_id, "name": name, "progress": progress})

    def _on_token_warning(self, used: int, limit: int, ratio: float) -> None:
        pct = int(ratio * 100)
        self._emit("token_warning", {"used": used, "limit": limit, "percent": pct})

    def _on_error(self, error: str) -> None:
        self._emit("error", {"error": error})

    def _on_idle(self) -> None:
        self._emit("idle", {})


class _DummyGui:
    """Fallback when launched standalone without SAO instance."""
    settings = None
    _ai_engine_actions = {}
    _plugin_manager = None
    _packet_bridge = None
    _mem_bridge = None
    _start_time = time.monotonic()


# ---------------------------------------------------------------------------
# Launcher
# ---------------------------------------------------------------------------

_running_window = None
_running_thread = None


def _html_path() -> str:
    return os.path.join(_ROOT, "web", "ai_editor_app.html")


def launch(gui_ref: Any = None, blocking: bool = False) -> None:
    """Open the AI Editor in a pywebview window.

    pywebview requires ``webview.start()`` on the main thread. When called from
    a Tk-hosted SAO instance (main thread occupied by Tk), we spawn a child
    process so pywebview gets its own main thread. The child process imports
    this module and calls ``launch(blocking=True)``.

    If *blocking* is True, runs synchronously (used by the child process and
    CLI ``python -m ai_editor.app``).
    """
    global _running_window, _running_thread

    if _running_window is not None:
        try:
            _running_window.show()
            return
        except Exception:
            _running_window = None

    if blocking:
        _launch_webview_blocking(gui_ref)
        return

    # Non-blocking: pywebview.start() needs main thread. If a Tk mainloop
    # already owns the main thread, spawn a subprocess instead.
    _launch_subprocess()


def _launch_subprocess() -> None:
    """Spawn a separate Python process for the AI Editor pywebview window."""
    global _running_thread
    import subprocess as _sp
    html_file = _html_path()
    if not os.path.isfile(html_file):
        print(f"[AIEditor] HTML not found: {html_file}")
        return
    cmd = [sys.executable, "-m", "ai_editor.app"]
    env = dict(os.environ)
    env.setdefault("PYTHONPATH", _ROOT)
    try:
        proc = _sp.Popen(cmd, cwd=_ROOT, env=env)
        print(f"[AIEditor] subprocess started (pid={proc.pid})")
    except Exception as exc:
        print(f"[AIEditor] subprocess failed: {exc}")


def _launch_webview_blocking(gui_ref: Any = None) -> None:
    """Run pywebview in the current thread (must be main thread)."""
    global _running_window
    import webview
    html_file = _html_path()
    if not os.path.isfile(html_file):
        print(f"[AIEditor] HTML not found: {html_file}")
        return
    api = AIEditorAPI(gui_ref)
    url = f"file:///{html_file.replace(os.sep, '/')}"
    window = webview.create_window(
        "SAO AI Editor",
        url=url,
        width=1200,
        height=800,
        min_size=(800, 500),
        js_api=api,
        frameless=True,
        easy_drag=False,
        text_select=True,
    )
    _running_window = window
    api.set_window(window)

    def _on_closed():
        global _running_window
        _running_window = None

    window.events.closed += _on_closed
    webview.start(debug=False)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    launch(blocking=True)
