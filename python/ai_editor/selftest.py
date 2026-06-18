"""AI Editor selftest — verifies all modules, Tk window build, and slash commands.

Run:  python -m ai_editor.selftest
"""

from __future__ import annotations

import json
import sys
import os
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

_PASS = 0
_FAIL = 0


def _check(label: str, ok: bool, detail: str = "") -> None:
    global _PASS, _FAIL
    if ok:
        _PASS += 1
        print(f"  ✓ {label}")
    else:
        _FAIL += 1
        print(f"  ✗ {label}" + (f" — {detail}" if detail else ""))


def test_imports() -> None:
    print("── Imports ──")
    try:
        from ai_editor.llm_engine import LLMEngine, ProviderConfig, Provider
        _check("llm_engine", True)
    except Exception as e:
        _check("llm_engine", False, str(e))

    try:
        from ai_editor.tool_registry import ToolRegistry, ToolDescriptor
        _check("tool_registry", True)
    except Exception as e:
        _check("tool_registry", False, str(e))

    try:
        from ai_editor.engine_tools import register_engine_tools
        _check("engine_tools", True)
    except Exception as e:
        _check("engine_tools", False, str(e))

    try:
        from ai_editor.chat_state import ChatController, Conversation, ChatMessage
        _check("chat_state", True)
    except Exception as e:
        _check("chat_state", False, str(e))

    try:
        from ai_editor.history import save_conversation, load_conversation, list_conversations, delete_conversation
        _check("history", True)
    except Exception as e:
        _check("history", False, str(e))

    try:
        from ai_editor.webview_bridge import AIEditorBridge
        _check("webview_bridge", True)
    except Exception as e:
        _check("webview_bridge", False, str(e))

    try:
        from ai_editor.agents import AgentDef, AgentRegistry, get_agent_registry
        _check("agents", True)
    except Exception as e:
        _check("agents", False, str(e))

    try:
        from ai_editor.workflows import WorkflowDef, WorkflowRegistry, WorkflowEngine, get_workflow_registry
        _check("workflows", True)
    except Exception as e:
        _check("workflows", False, str(e))

    try:
        from ai_editor.scopes import resolve_scopes, effective_permissions, tool_permission, MODES
        _check("scopes", True)
    except Exception as e:
        _check("scopes", False, str(e))

    try:
        from ai_editor.extension_host import (
            ExtensionDescription, ExtensionRegistry, ExtensionScanner,
            ExtensionActivator, CommandService, ExtensionContext,
            ExtensionPoints, ExtensionHost,
            Position, Range, Uri, Disposable, EventEmitter,
        )
        _check("extension_host", True)
    except Exception as e:
        _check("extension_host", False, str(e))

    try:
        from ai_editor.vscode_api import (
            VscodeNamespace, LanguageModelChat, ChatParticipant,
            ChatRequest, ChatContext, ChatResponseStream,
            WorkspaceConfiguration, AuthenticationSession,
        )
        _check("vscode_api", True)
    except Exception as e:
        _check("vscode_api", False, str(e))

    try:
        from ai_editor.auth import AuthService, AuthSession, get_auth_service
        _check("auth", True)
    except Exception as e:
        _check("auth", False, str(e))

    try:
        from ai_editor.claude_proxy import ClaudeProxy
        _check("claude_proxy", True)
    except Exception as e:
        _check("claude_proxy", False, str(e))

    try:
        from gui_modules.sao_gui_ai_editor import AIEditorPanel
        _check("sao_gui_ai_editor", True)
    except Exception as e:
        _check("sao_gui_ai_editor", False, str(e))


class _FakeGui:
    settings = None
    _ai_engine_actions = {"sample_state": lambda **kw: {"ok": True, "source": "plugin"}}
    _plugin_manager = None
    _packet_bridge = None
    _mem_bridge = None
    _start_time = 0.0


class _FakeSettings:
    def __init__(self, data):
        self.data = data
        self.saved = False

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value

    def save(self):
        self.saved = True


class _SettingsGui(_FakeGui):
    def __init__(self, data):
        self.settings = _FakeSettings(data)
        self._ai_engine_actions = dict(_FakeGui._ai_engine_actions)


def test_app_settings_parity() -> None:
    print("── App Settings Parity ──")
    from ai_editor.app import AIEditorAPI

    api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    js_methods = (
        "load_config", "save_config", "list_tools", "confirm_tool",
        "send_image_message", "count_conversation_tokens", "send_message",
        "cancel", "new_chat", "export_chat", "execute_tool",
        "list_history", "get_instructions", "save_user_instructions",
        "save_instruction_file", "delete_instruction_file", "list_agents",
        "list_workflows", "get_active_agent", "set_active_agent",
        "clear_active_agent", "delete_agent", "delete_workflow",
        "run_workflow", "get_scopes", "save_agent", "save_workflow",
        "get_chat_controls", "set_active_provider", "set_active_model",
        "set_active_mode", "set_provider_model", "set_chat_provider",
        "set_chat_controls",
        "load_history", "switch_provider", "list_chat_providers",
        "provider_send", "provider_cancel", "provider_new_chat",
        "get_model_info", "test_connection", "set_mode", "save_config",
        "get_mode", "list_models", "save_custom_model",
        "delete_custom_model", "search_extensions",
        "list_installed_extensions", "uninstall_extension",
        "install_extension", "win_minimize", "win_maximize", "win_close",
    )
    missing = [name for name in js_methods if not callable(getattr(api, name, None))]
    _check("AIEditorAPI JS-callable methods", not missing, ", ".join(missing))

    data = {
        "ai_editor": {
            "provider": "openai",
            "api_key": "old-key",
            "user_instructions": "keep me",
            "provider_keys": {"anthropic": "old-claude"},
            "custom_models": {"kept-model": {"max_input": 123, "max_output": 45}},
            "mode": "agent",
            "permissions": {"readFile": "disabled"},
            "claude_code": {"cli_path": "claude-cli"},
            "future_section": {"enabled": True},
        },
        "panel_themes": {"act": "light"},
    }
    gui = _SettingsGui(data)
    api = AIEditorAPI(gui)
    loaded = api.load_config()
    advanced_keys = (
        "top_p", "frequency_penalty", "presence_penalty", "stop",
        "max_input_tokens", "max_output_tokens", "timeout",
        "extra_headers", "extra_body", "provider_keys", "_provider_keys",
        "custom_models", "mode", "permissions",
    )
    _check("load_config advanced fields", all(k in loaded for k in advanced_keys))
    _check("load_config existing safe section",
           loaded.get("claude_code", {}).get("cli_path") == "claude-cli")
    _check("load_config skips absent safe section", "codex" not in loaded)

    payload = {
        "provider": "custom",
        "api_key": "new-key",
        "base_url": "https://example.invalid/v1",
        "model": "phase-one-model",
        "temperature": "0.25",
        "max_tokens": "8192",
        "top_p": "0.82",
        "frequency_penalty": "0.15",
        "presence_penalty": "-0.2",
        "stop": "END, STOP",
        "max_input_tokens": "64000",
        "max_output_tokens": "2048",
        "timeout": "45",
        "extra_headers": {"X-Test": "1"},
        "extra_body": {"stream_options": {"include_usage": True}},
        "_provider_keys": {"openai": "new-openai", "deepseek": "new-deepseek"},
        "unknown_payload": {"preserve": True},
    }
    result = api.save_config(payload)
    stored = gui.settings.data["ai_editor"]
    loaded = api.load_config()
    _check("save_config ok", result.get("ok") is True and gui.settings.saved)
    _check("save_config preserves siblings",
           stored.get("user_instructions") == "keep me"
           and stored.get("future_section") == {"enabled": True}
           and stored.get("claude_code", {}).get("cli_path") == "claude-cli")
    _check("advanced settings round-trip",
           loaded.get("provider") == "custom"
           and abs(loaded.get("top_p", 0) - 0.82) < 0.0001
           and abs(loaded.get("frequency_penalty", 0) - 0.15) < 0.0001
           and abs(loaded.get("presence_penalty", 0) + 0.2) < 0.0001
           and loaded.get("stop") == ["END", "STOP"]
           and loaded.get("max_input_tokens") == 64000
           and loaded.get("max_output_tokens") == 2048
           and loaded.get("timeout") == 45
           and loaded.get("extra_headers") == {"X-Test": "1"}
           and loaded.get("extra_body") == {"stream_options": {"include_usage": True}})
    _check("provider keys and unknown keys preserved",
           loaded.get("_provider_keys", {}).get("deepseek") == "new-deepseek"
           and stored.get("unknown_payload") == {"preserve": True})

    malformed = _SettingsGui({
        "ai_editor": {
            "provider_keys": "broken",
            "_provider_keys": {"anthropic": "legacy-key"},
            "top_p": "nan",
            "timeout": "never",
            "stop": "A,, B",
            "extra_headers": ["bad"],
            "mode": ["bad"],
            "permissions": "bad",
            "terminal": "bad",
        }
    })
    loaded_bad = AIEditorAPI(malformed).load_config()
    _check("malformed settings normalized safely",
           loaded_bad.get("provider_keys") == {"anthropic": "legacy-key"}
           and loaded_bad.get("top_p") == 1.0
           and loaded_bad.get("timeout") == 180
           and loaded_bad.get("stop") == ["A", "B"]
           and loaded_bad.get("extra_headers") == {}
           and loaded_bad.get("mode") == "agent"
           and loaded_bad.get("permissions") == {}
           and loaded_bad.get("terminal", {}).get("profile"))

    provider_gui = _SettingsGui({"ai_editor": {
        "provider_keys": {"openai": "test-openai"},
        "codex": {"model": "codex-test", "transport": "responses"},
    }})
    provider_api = AIEditorAPI(provider_gui)
    provider_api._ensure_engine()
    codex = provider_api._provider_registry.get("codex")
    ctrl = provider_api._create_provider_controller(codex)
    _check("codex provider settings applied",
           ctrl.engine.config.model == "codex-test"
           and ctrl.engine.config.transport == "responses")

    provider_gui.settings.data["ai_editor"]["codex"]["transport"] = "cli"
    _check("unsupported CLI transport rejected",
           "CLI transport" in provider_api._unsupported_provider_transport(codex))

    mode_data = {"ai_editor": {"mode": "ask", "permissions": {"readFile": "allowed"}}}
    mode_api = AIEditorAPI(_SettingsGui(mode_data))
    mode_api._ensure_engine()
    mode = mode_api.get_mode("plan")
    _check("get_mode returns defaults and overrides",
           mode.get("mode") == "ask"
           and mode.get("selected_mode") == "plan"
           and mode.get("defaults", {}).get("runTerminal") == "confirm"
           and mode.get("overrides", {}).get("readFile") == "allowed")

    mode_api.set_mode("plan")
    direct_result = json.loads(mode_api.execute_tool(
        "runTerminal", json.dumps({"command": "echo ok"})))
    _check("direct mutating tool requires confirmation",
           direct_result.get("requires_confirmation") is True)

    from ai_editor.mcp_client import load_mcp_configs
    mcp_payload = {"ai_editor": {"mcp": {"autostart": False, "servers": [
        {"id": "local", "transport": "stdio", "command": "cmd"}
    ]}}}
    getter = lambda key, default=None: mcp_payload.get(key, default)
    _check("mcp autostart default prevents launch", load_mcp_configs(getter) == [])
    mcp_payload["ai_editor"]["mcp"]["autostart"] = True
    _check("mcp autostart loads configured server",
           [c.id for c in load_mcp_configs(getter)] == ["local"])
    mcp_payload["ai_editor"]["mcp"]["access"] = "disabled"
    _check("mcp disabled blocks configs", load_mcp_configs(getter) == [])

    ext_api = AIEditorAPI(_SettingsGui({"ai_editor": {
        "extensions": {"confirm_install": True, "blocked_publishers": ["blocked"]}
    }}))
    _check("extension install requires confirmation",
           ext_api.install_extension("allowed.sample").get("requires_confirmation") is True)
    _check("blocked extension publisher rejected",
           "blocked" in ext_api.install_extension("blocked.sample", confirmed=True).get("error", ""))

    controls_gui = _SettingsGui({"ai_editor": {
        "provider": "openai",
        "api_key": "old-key",
        "model": "gpt-4o",
        "provider_keys": {"anthropic": "old-claude"},
        "custom_models": {"toolbar-model": {"max_input": 321, "max_output": 45}},
        "mode": "plan",
        "permissions": {"readFile": "allowed"},
        "future_section": {"enabled": True},
    }})
    controls_api = AIEditorAPI(controls_gui)
    controls = controls_api.get_chat_controls()
    _check("get_chat_controls payload includes selectors",
           controls.get("provider") == "openai"
           and controls.get("model") == "gpt-4o"
           and controls.get("mode") == "plan"
           and isinstance(controls.get("providers"), list)
           and any(m.get("id") == "toolbar-model" for m in controls.get("models", []))
           and isinstance(controls.get("agents"), list)
           and isinstance(controls.get("workflows"), list)
           and controls.get("context_window", {}).get("max_input") == 128000)

    provider_result = controls_api.set_active_provider("anthropic")
    stored_controls = controls_gui.settings.data["ai_editor"]
    _check("set_active_provider updates engine and preserves settings",
           provider_result.get("ok") is True
           and controls_api._engine.config.provider == "anthropic"
           and controls_api._engine.config.model == "claude-sonnet-4-20250514"
           and stored_controls.get("provider") == "anthropic"
           and stored_controls.get("model") == "claude-sonnet-4-20250514"
           and stored_controls.get("provider_keys", {}).get("anthropic") == "old-claude"
           and stored_controls.get("future_section") == {"enabled": True})

    model_result = controls_api.set_active_model("toolbar-model")
    mode_result = controls_api.set_active_mode("agent")
    _check("active chat setters refresh controls",
           model_result.get("controls", {}).get("model") == "toolbar-model"
           and model_result.get("controls", {}).get("status", {}).get("model") == "toolbar-model"
           and controls_api._engine.config.model == "toolbar-model"
           and mode_result.get("controls", {}).get("mode") == "agent")

    chat_provider_result = controls_api.set_chat_provider("chat")
    _check("set_chat_provider switches active provider tab",
           chat_provider_result.get("ok") is True
           and chat_provider_result.get("controls", {}).get("active_chat_provider") == "chat")

    aggregate_result = controls_api.set_chat_controls({
        "provider": "openai", "model": "gpt-4o-mini", "mode": "plan",
        "agent_id": "code-reviewer", "active_chat_provider": "chat",
    })
    _check("set_chat_controls aggregate setter updates state",
           aggregate_result.get("ok") is True
           and aggregate_result.get("controls", {}).get("provider") == "openai"
           and aggregate_result.get("controls", {}).get("model") == "gpt-4o-mini"
           and aggregate_result.get("controls", {}).get("mode") == "plan"
           and aggregate_result.get("controls", {}).get("active_agent_id") == "code-reviewer")

    provider_model_result = controls_api.set_provider_model("anthropic", "claude-test")
    _check("set_provider_model compatibility alias updates provider and model",
           provider_model_result.get("ok") is True
           and provider_model_result.get("controls", {}).get("provider") == "anthropic"
           and provider_model_result.get("controls", {}).get("model") == "claude-test")


def test_phase1_ai_editor_regressions() -> None:
    print("── Phase 1 AI Editor Regressions ──")
    from ai_editor.app import AIEditorAPI
    from ai_editor.chat_providers import ChatProviderRegistry
    from ai_editor.scopes import tool_permission

    eval_args = json.dumps({"action": "eval", "expression": "2 + 2"})

    chat_api = AIEditorAPI(_SettingsGui({"ai_editor": {"mode": "ask"}}))
    chat_eval = json.loads(chat_api.execute_tool("engine", eval_args, confirmed=True))
    _check("ask mode blocks direct engine(eval)",
           "Engine action disabled" in chat_eval.get("error", ""))

    plan_api = AIEditorAPI(_SettingsGui({"ai_editor": {"mode": "plan"}}))
    plan_eval = json.loads(plan_api.execute_tool("engine", eval_args))
    _check("plan mode requires confirmation for direct engine(eval)",
           plan_eval.get("requires_confirmation") is True)

    agent_api = AIEditorAPI(_SettingsGui({"ai_editor": {"mode": "agent"}}))
    agent_eval = json.loads(agent_api.execute_tool("engine", eval_args, confirmed=True))
    _check("agent mode can run confirmed direct engine(eval)",
           agent_eval.get("result") == 4)

    _check("unknown tool default permissions by mode",
           tool_permission("ask", "customPhaseTool") == "disabled"
           and tool_permission("plan", "customPhaseTool") == "confirm"
           and tool_permission("agent", "customPhaseTool") == "allowed")
    _check("unknown tool explicit overrides honored",
           tool_permission("ask", "customPhaseTool", {"customPhaseTool": "allowed"}) == "allowed"
           and tool_permission("plan", "customPhaseTool", {"customPhaseTool": "disabled"}) == "disabled"
           and tool_permission("agent", "customPhaseTool", {"customPhaseTool": "allowed"}) == "allowed")

    custom_api = AIEditorAPI(_SettingsGui({"ai_editor": {"mode": "agent"}}))
    custom_api._ensure_engine()
    custom_api._registry.register(
        "customPhaseTool", "Custom phase test tool", {"type": "object"},
        lambda **kw: {"ok": True}, category="custom")
    custom_api._apply_mode_permissions()
    custom_tool = custom_api._registry.get("customPhaseTool")
    custom_default = json.loads(custom_api.execute_tool("customPhaseTool", "{}"))
    _check("custom tool auto-allowed in agent mode",
           custom_tool is not None
           and custom_tool.requires_confirm is False
           and custom_default.get("ok") is True)
    agent_preview = custom_api.get_mode("agent")
    ask_preview = custom_api.get_mode("ask")
    _check("get_mode returns computed dynamic tool permissions",
        agent_preview.get("permissions", {}).get("customPhaseTool") == "allowed"
        and agent_preview.get("defaults", {}).get("customPhaseTool") == "allowed"
        and ask_preview.get("permissions", {}).get("customPhaseTool") == "disabled")
    custom_api._confirmation_timeout = 0.0
    custom_api._mode = "plan"
    confirm_events = []
    custom_api._emit = lambda event, data: confirm_events.append((event, data))
    denied = custom_api._on_tool_confirm("phase-call", "customPhaseTool", "{}")
    stale = custom_api.confirm_tool("phase-call", True)
    provider_denied = custom_api._on_provider_tool_confirm(
     "codex", "provider-phase-call", "customPhaseTool", "{}")
    confirm_only = [e for e in confirm_events if e[0].endswith("_confirm")]
    _check("tool confirmation defaults fail closed and provider-scoped",
        denied is False
        and provider_denied is False
        and stale.get("error") == "No pending confirmation"
        and confirm_only[0][0] == "tool_confirm"
        and confirm_only[-1][0] == "provider_tool_confirm"
        and confirm_only[-1][1].get("provider") == "codex")
    custom_api.set_tool_permission("customPhaseTool", "allowed")
    custom_allowed = json.loads(custom_api.execute_tool("customPhaseTool", "{}"))
    _check("custom tool override allowed runs",
           custom_allowed.get("ok") is True)

    def flags(settings):
        getter = lambda key, default=None: settings.get(key, default)
        return {p["id"]: p["available"]
                for p in ChatProviderRegistry().list_available(getter)}

    cli_only_settings = {"ai_editor": {
        "claude_code": {"prefer_cli": True, "cli_path": "claude"},
        "codex": {"transport": "cli", "cli_path": "codex"},
        "provider_keys": {"openai": "test-openai"},
    }}
    registry_cli_flags = flags(cli_only_settings)
    _check("ChatProviderRegistry rejects unsupported CLI transports",
           registry_cli_flags.get("claude-code") is False
           and registry_cli_flags.get("codex") is False)

    api_cli = AIEditorAPI(_SettingsGui(cli_only_settings))
    api_cli_flags = {p["id"]: p["available"]
                     for p in api_cli.list_chat_providers().get("providers", [])}
    _check("list_chat_providers reports CLI transports unavailable",
           api_cli_flags.get("claude-code") is False
           and api_cli_flags.get("codex") is False)

    api_key_settings = {"ai_editor": {
        "claude_code": {"prefer_cli": False},
        "codex": {"transport": "responses"},
        "provider_keys": {"anthropic": "test-anthropic", "openai": "test-openai"},
    }}
    registry_key_flags = flags(api_key_settings)
    _check("ChatProviderRegistry keeps API-key transports available",
           registry_key_flags.get("claude-code") is True
           and registry_key_flags.get("codex") is True)


def test_tool_registry() -> None:
    print("── Tool Registry ──")
    from ai_editor.tool_registry import ToolRegistry
    from ai_editor.engine_tools import register_engine_tools

    reg = ToolRegistry()
    register_engine_tools(reg, _FakeGui())

    tools = reg.list_tools()
    _check(f"tools registered: {len(tools)}", len(tools) >= 10)

    cats = reg.categories()
    _check(f"categories: {cats}", len(cats) >= 4)

    schemas = reg.to_openai_tools()
    _check(f"OpenAI schemas: {len(schemas)}", len(schemas) == len(tools))

    # Execute engine aggregate tool
    result = reg.execute("engine", json.dumps({"action": "sample_state"}))
    data = json.loads(result)
    _check(f"plugin action exec: ok={data.get('ok')}", data.get("ok") is True)

    result = reg.execute("engine", json.dumps({"action": "system_info"}))
    data = json.loads(result)
    _check(f"engine(system_info) exec", "version" in data)

    result = reg.execute("engine", json.dumps({"action": "eval", "expression": "2 + 2"}))
    data = json.loads(result)
    _check(f"engine(eval): 2+2={data.get('result')}", data.get("result") == 4)

    result = reg.execute("nonexistent_tool", "{}")
    data = json.loads(result)
    _check("unknown tool error", "error" in data)


def test_llm_engine() -> None:
    print("── LLM Engine ──")
    from ai_editor.llm_engine import LLMEngine, ProviderConfig, Provider

    # OpenAI config
    e = LLMEngine(ProviderConfig(provider="openai", api_key="test"))
    _check("OpenAI base_url", "openai.com" in e.config.effective_base_url)
    _check("OpenAI model", e.config.effective_model == "gpt-4o")
    _check("Not Anthropic native", not e._is_anthropic_native(e.config))

    # Anthropic config
    e2 = LLMEngine(ProviderConfig(provider="anthropic", api_key="test"))
    _check("Anthropic base_url", "anthropic.com" in e2.config.effective_base_url)
    _check("Anthropic native", e2._is_anthropic_native(e2.config))

    # Anthropic body
    msgs = [{"role": "system", "content": "Hello"}, {"role": "user", "content": "Hi"}]
    body = e2._build_body(e2.config, msgs, None, stream=True)
    _check("Anthropic system extracted", "system" in body and body["system"] == "Hello")
    _check("Anthropic msgs filtered", len(body["messages"]) == 1)

    # DeepSeek config
    e3 = LLMEngine(ProviderConfig(provider="deepseek"))
    _check("DeepSeek base_url", "deepseek.com" in e3.config.effective_base_url)

    # Ollama config
    e4 = LLMEngine(ProviderConfig(provider="ollama"))
    _check("Ollama base_url", "localhost" in e4.config.effective_base_url)


def test_conversation() -> None:
    print("── Conversation ──")
    from ai_editor.chat_state import Conversation, ChatMessage
    from ai_editor.llm_engine import ToolCall

    conv = Conversation(system_prompt="Test system")
    conv.add_message(ChatMessage(role="user", content="Hello"))
    conv.add_message(ChatMessage(role="assistant", content="Hi", tool_calls=[
        ToolCall(id="tc1", name="get_time", arguments='{"tz":"UTC"}'),
    ]))
    conv.add_message(ChatMessage(role="tool", content='{"time":"12:00"}', tool_call_id="tc1"))

    msgs = conv.to_api_messages()
    _check(f"API messages count: {len(msgs)}", len(msgs) == 4)  # system + 3
    _check("System message", msgs[0]["role"] == "system")
    _check("Tool calls in assistant", "tool_calls" in msgs[2])
    _check("Tool result", msgs[3]["role"] == "tool")
    _check("Title auto-set", conv.title == "Hello")


def test_history() -> None:
    print("── History ──")
    from ai_editor.history import save_conversation, load_conversation, list_conversations, delete_conversation

    cid = "_selftest_temp"
    save_conversation(cid, "Selftest", [{"role": "user", "content": "test"}])

    data = load_conversation(cid)
    _check("Load saved", data is not None and data["title"] == "Selftest")

    entries = list_conversations()
    _check("Listed", any(e["id"] == cid for e in entries))

    delete_conversation(cid)
    _check("Deleted", load_conversation(cid) is None)


def test_bridge() -> None:
    print("── WebView Bridge ──")
    from ai_editor.webview_bridge import AIEditorBridge

    events = []
    bridge = AIEditorBridge(_FakeGui(), lambda n, p: events.append((n, p)))

    result = bridge.handle_command("ai_editor_load_config", {})
    _check("load_config", "provider" in result)

    result = bridge.handle_command("ai_editor_list_tools", {})
    _check(f"list_tools: {len(result.get('tools', []))}", len(result.get("tools", [])) >= 10)

    result = bridge.handle_command("ai_editor_list_history", {})
    _check("list_history", "entries" in result)

    result = bridge.handle_command("ai_editor_get_instructions", {})
    _check("get_instructions", "user_instructions" in result and "files" in result)

    result = bridge.handle_command("unknown_cmd", {})
    _check("unknown cmd", "error" in result)


def test_instructions() -> None:
    print("── Instructions ──")
    import tempfile, shutil
    from ai_editor.prompts import (
        load_instructions, list_instruction_files,
        save_instruction_file, delete_instruction_file, get_system_prompt,
    )

    tmpdir = tempfile.mkdtemp(prefix="sao_inst_test_")
    try:
        # Empty initially
        inst = load_instructions(workspace_root=tmpdir)
        _check("empty instructions", inst == "")

        files = list_instruction_files(workspace_root=tmpdir)
        _check("no files initially", len(files) == 0)

        # Save project-level instructions.md
        r = save_instruction_file("instructions.md", "Be concise.", workspace_root=tmpdir)
        _check("save project inst", r.get("ok") is True)

        files = list_instruction_files(workspace_root=tmpdir)
        _check("project file listed", len(files) == 1 and files[0]["scope"] == "workspace")

        # Save per-file instruction
        r = save_instruction_file("coding-style.md", "Use 4 spaces.", workspace_root=tmpdir)
        _check("save coding-style", r.get("ok") is True)

        files = list_instruction_files(workspace_root=tmpdir)
        _check("2 files listed", len(files) == 2)

        # Load combined
        inst = load_instructions(workspace_root=tmpdir)
        _check("combined has project text", "Be concise" in inst)
        _check("combined has file text", "4 spaces" in inst)

        # User instructions via settings_getter
        def fake_settings(key, default=None):
            if key == "ai_editor":
                return {"user_instructions": "Always explain code"}
            return default
        inst = load_instructions(settings_getter=fake_settings, workspace_root=tmpdir)
        _check("user instructions included", "Always explain" in inst)

        # get_system_prompt with instructions
        sp = get_system_prompt(settings_getter=fake_settings)
        _check("system prompt includes custom", "Custom Instructions" in sp)

        # Delete file
        r = delete_instruction_file("coding-style.md", workspace_root=tmpdir)
        _check("delete file", r.get("ok") is True)
        files = list_instruction_files(workspace_root=tmpdir)
        _check("1 file after delete", len(files) == 1)

        # Delete nonexistent
        r = delete_instruction_file("nope.md", workspace_root=tmpdir)
        _check("delete nonexistent", r.get("ok") is False)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_scopes() -> None:
    print("── Scopes ──")
    from ai_editor.scopes import (
        resolve_scopes, scope_subdirs, effective_permissions,
        tool_permission, MODES, MODE_PERMISSIONS,
    )

    scopes = resolve_scopes()
    _check(f"scopes resolved: {len(scopes)}", len(scopes) >= 2)
    _check("system scope first", scopes[0]["scope"] == "system")
    _check("workspace scope second", scopes[1]["scope"] == "workspace")

    agent_dirs = scope_subdirs("agents")
    _check("agent subdirs", len(agent_dirs) >= 2)
    _check("agents subdir path", agent_dirs[0]["path"].endswith("agents"))

    # Mode permissions
    _check("3 modes", len(MODES) == 3)

    ask_perms = effective_permissions("ask")
    _check("ask: readFile allowed", ask_perms.get("readFile") == "allowed")
    _check("ask: editFile disabled", ask_perms.get("editFile") == "disabled")
    _check("ask: engine allowed", ask_perms.get("engine") == "allowed")

    plan_perms = effective_permissions("plan")
    _check("plan: readFile allowed", plan_perms.get("readFile") == "allowed")
    _check("plan: editFile confirm", plan_perms.get("editFile") == "confirm")

    agent_perms = effective_permissions("agent")
    _check("agent: editFile allowed", agent_perms.get("editFile") == "allowed")

    # Overrides
    custom = effective_permissions("plan", {"readFile": "disabled"})
    _check("override readFile", custom["readFile"] == "disabled")

    # tool_permission helper
    _check("tool_permission ask/readFile",
           tool_permission("ask", "readFile") == "allowed")
    _check("tool_permission plan/readFile",
           tool_permission("plan", "readFile") == "allowed")
    _check("tool_permission unknown tool defaults confirm in plan",
           tool_permission("plan", "some_unknown_tool") == "confirm")
    _check("tool_permission unknown tool defaults allowed in agent",
           tool_permission("agent", "some_unknown_tool") == "allowed")
    # Backward compat aliases
    from ai_editor.scopes import normalize_mode
    _check("normalize_mode chat->ask", normalize_mode("chat") == "ask")
    _check("normalize_mode edit->plan", normalize_mode("edit") == "plan")
    _check("normalize_mode agent stays", normalize_mode("agent") == "agent")


def test_extension_host() -> None:
    print("── Extension Host ──")
    import tempfile, shutil
    from ai_editor.extension_host import (
        ExtensionDescription, ExtensionRegistry, ExtensionScanner,
        ExtensionActivator, CommandService, ExtensionContext,
        ExtensionPoints, ExtensionHost,
        Position, Range, Uri, Disposable, EventEmitter,
    )

    # Types
    pos = Position(1, 5)
    _check("Position", pos.line == 1 and pos.character == 5)
    rng = Range(Position(0, 0), Position(10, 0))
    _check("Range", rng.end.line == 10)
    uri = Uri.file("C:/test/file.py")
    _check("Uri.file", uri.scheme == "file" and "test" in uri.path)
    uri2 = Uri.parse("https://example.com/api")
    _check("Uri.parse", uri2.scheme == "https")
    d = Disposable(lambda: None)
    d.dispose()
    _check("Disposable", True)
    ee = EventEmitter()
    fired = []
    ee.event(lambda v: fired.append(v))
    ee.fire(42)
    _check("EventEmitter", fired == [42])

    # ExtensionDescription from package.json
    pkg = {
        "name": "test-ext", "publisher": "test",
        "version": "1.0.0", "displayName": "Test Extension",
        "activationEvents": ["onCommand:test.hello"],
        "contributes": {
            "commands": [{"command": "test.hello", "title": "Hello"}],
            "chatParticipants": [{"id": "test-chat", "name": "TestChat",
                                  "fullName": "Test Chat Bot"}],
            "languageModelTools": [{"name": "test_tool",
                                    "displayName": "Test Tool"}],
        },
    }
    desc = ExtensionDescription.from_package_json(pkg, "/fake/path")
    _check("ExtDesc.id", desc.id == "test.test-ext")
    _check("ExtDesc.activation", "onCommand:test.hello" in desc.activation_events)
    _check("ExtDesc.to_dict", desc.to_dict()["displayName"] == "Test Extension")

    # Registry
    reg = ExtensionRegistry()
    reg.register(desc)
    _check("Registry.get", reg.get("test.test-ext") is not None)
    _check("Registry.list_all", len(reg.list_all()) == 1)
    targets = reg.get_for_activation_event("onCommand:test.hello")
    _check("Registry.activation_map", len(targets) == 1)

    # CommandService
    cmds = CommandService()
    result_box = []
    dispose = cmds.register("test.hello", lambda: result_box.append("ok") or "done")
    _check("Cmd.has", cmds.has("test.hello"))
    r = cmds.execute("test.hello")
    _check("Cmd.execute", r == "done" and result_box == ["ok"])
    _check("Cmd.list", "test.hello" in cmds.list_commands())
    dispose()
    _check("Cmd.dispose", not cmds.has("test.hello"))

    # ExtensionPoints
    ep = ExtensionPoints(cmds)
    ep.process(desc)
    _check("EP.chatParticipants", len(ep.chat_participants) == 1)
    _check("EP.lm_tools", len(ep.language_model_tools) == 1)
    _check("EP.commands registered", cmds.has("test.hello"))
    summary = ep.to_summary()
    _check("EP.summary", summary["chatParticipants"] == 1)

    # ExtensionContext
    tmpdir = tempfile.mkdtemp(prefix="sao_ext_test_")
    try:
        desc2 = ExtensionDescription(id="test.ctx", extension_path=tmpdir)
        ctx = ExtensionContext(desc2, os.path.join(tmpdir, ".storage"))
        ctx.global_state.update("key1", "value1")
        _check("Ctx.globalState.get", ctx.global_state.get("key1") == "value1")
        ctx.secrets.store("token", "abc123")
        _check("Ctx.secrets.get", ctx.secrets.get("token") == "abc123")

        # Scanner with temp extension
        ext_dir = os.path.join(tmpdir, "extensions", "my-ext")
        os.makedirs(ext_dir)
        with open(os.path.join(ext_dir, "package.json"), "w") as f:
            json.dump({"name": "my-ext", "publisher": "test",
                       "version": "0.1.0", "activationEvents": ["*"]}, f)
        found = ExtensionScanner.scan_directory(os.path.join(tmpdir, "extensions"))
        _check("Scanner.scan", len(found) == 1 and found[0].id == "test.my-ext")

        # Full pipeline: ExtensionHost
        host = ExtensionHost()
        count = host.scan([os.path.join(tmpdir, "extensions")])
        _check("Host.scan", count == 1)
        activated = host.start()
        _check("Host.start", len(activated) >= 1)
        _check("Host.is_activated", host.activator.is_activated("test.my-ext"))
        exts = host.list_extensions()
        _check("Host.list", len(exts) == 1 and exts[0]["activated"])
        host.shutdown()
        _check("Host.shutdown", not host.activator.is_activated("test.my-ext"))

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_vscode_api() -> None:
    print("── VSCode API ──")
    from ai_editor.extension_host import ExtensionHost
    from ai_editor.vscode_api import (
        VscodeNamespace, LanguageModelChat, ChatParticipant,
        ChatRequest, ChatContext, ChatResponseStream, ChatResult,
        WorkspaceConfiguration, LanguageModelToolResult,
    )

    host = ExtensionHost()
    ns = VscodeNamespace(host)
    api = ns.build()

    # Namespace structure
    _check("api.commands", "registerCommand" in api["commands"])
    _check("api.window", "showInformationMessage" in api["window"])
    _check("api.workspace", "getConfiguration" in api["workspace"])
    _check("api.env", api["env"]["appName"] == "SAO AI Editor")
    _check("api.lm", "selectChatModels" in api["lm"])
    _check("api.chat", "createChatParticipant" in api["chat"])
    _check("api.authentication", "getSession" in api["authentication"])

    # Types
    _check("api.Position", api["Position"] is not None)
    _check("api.Uri", api["Uri"] is not None)
    _check("api.Disposable", api["Disposable"] is not None)

    # Commands via namespace
    box = []
    api["commands"]["registerCommand"]("test.api", lambda: box.append(1) or "ok")
    r = api["commands"]["executeCommand"]("test.api")
    _check("ns.commands.execute", r == "ok" and box == [1])

    # Chat participant
    handler_calls = []
    def my_handler(req, ctx, stream, token):
        handler_calls.append(req.prompt)
        stream.markdown("**Hello**")
        return ChatResult()
    cp = api["chat"]["createChatParticipant"]("test-bot", my_handler)
    _check("createChatParticipant", cp.id == "test-bot")
    _check("participant in registry", "test-bot" in ns.chat_participants)

    req = ChatRequest(prompt="Hi there")
    ctx = ChatContext()
    stream = ChatResponseStream()
    cp.request_handler(req, ctx, stream, None)
    _check("participant.handler called", handler_calls == ["Hi there"])
    _check("stream.content", stream.get_content() == "**Hello**")

    # LM tool
    class MyTool:
        def invoke(self, options, token):
            return LanguageModelToolResult.text(f"result:{options.input}")
    dispose = api["lm"]["registerTool"]("my_tool", MyTool())
    _check("registerTool", "my_tool" in ns.registered_tools)
    dispose.dispose()
    _check("tool disposed", "my_tool" not in ns.registered_tools)

    # Variable
    api["chat"]["registerVariable"]("testvar", "A test variable",
                                     lambda: "var_value")
    _check("registerVariable", "testvar" in ns.variables)

    # Configuration
    cfg = api["workspace"]["getConfiguration"]("ai_editor")
    _check("getConfiguration", isinstance(cfg, WorkspaceConfiguration))

    # selectChatModels (no engine)
    models = api["lm"]["selectChatModels"]()
    _check("selectChatModels (no engine)", len(models) == 0)

    # LanguageModelChat
    lm = LanguageModelChat(id="test-model", name="Test", vendor="test",
                            max_input_tokens=100000)
    _check("LM.countTokens", lm.count_tokens("Hello world") > 0)
    _check("LM.capabilities", lm.capabilities.supports_tool_calling is True)

    # New types from gap analysis
    from ai_editor.vscode_api import (
        LanguageModelTextPart, LanguageModelToolCallPart,
        LanguageModelDataPart, LanguageModelThinkingPart,
        LanguageModelChatMessage as LMChatMsg, LanguageModelError,
        CancellationTokenSource, CancellationToken,
    )

    # Message types
    user_msg = LMChatMsg.User("Hello")
    _check("LMChatMsg.User", user_msg.role == 1)
    asst_msg = LMChatMsg.Assistant([LanguageModelTextPart("reply")])
    _check("LMChatMsg.Assistant parts", len(asst_msg.content) == 1)
    sys_msg = LMChatMsg.System("system prompt")
    _check("LMChatMsg.System", sys_msg.role == 0)
    api_dict = user_msg.to_api_dict()
    _check("LMChatMsg.to_api_dict", api_dict["role"] == "user")

    # Part types
    tp = LanguageModelTextPart(value="hello")
    _check("TextPart", tp.value == "hello")
    tcp = LanguageModelToolCallPart(call_id="c1", name="tool1")
    _check("ToolCallPart", tcp.name == "tool1")
    dp = LanguageModelDataPart.image(b"\x89PNG", "image/png")
    _check("DataPart.image", dp.mime_type == "image/png")
    thp = LanguageModelThinkingPart(value="thinking...")
    _check("ThinkingPart", thp.value == "thinking...")

    # Error types
    err = LanguageModelError.NotFound("test")
    _check("LMError.NotFound", err.code == "NotFound")
    err2 = LanguageModelError.NoPermissions()
    _check("LMError.NoPermissions", err2.code == "NoPermissions")
    err3 = LanguageModelError.Blocked()
    _check("LMError.Blocked", err3.code == "Blocked")

    # CancellationToken
    cts = CancellationTokenSource()
    tok = cts.token
    _check("CancellationToken.initial", not tok.is_cancellation_requested)
    cancelled_flag = []
    tok.on_cancellation_requested(lambda: cancelled_flag.append(True))
    cts.cancel()
    _check("CancellationToken.cancelled", tok.is_cancellation_requested)
    _check("CancellationToken.listener", len(cancelled_flag) == 1)
    _check("CancellationToken.NONE", not CancellationToken.NONE.is_cancellation_requested)

    # ChatResponseStream extended methods
    stream2 = ChatResponseStream()
    stream2.anchor("http://example.com", "Example")
    stream2.button({"command": "test"})
    stream2.warning("Be careful")
    stream2.info("FYI")
    stream2.progress("Working...")
    stream2.filetree({"items": []})
    stream2.thinking_progress("hmm...")
    _check("stream.anchor+warn+info", "Example" in stream2.get_content() and "careful" in stream2.get_content())

    # ChatRequest extended fields
    req2 = ChatRequest(prompt="test", tool_references=[{"name": "tool1"}],
                        tool_invocation_token="tok123", attempt=2)
    _check("ChatRequest.toolInvocationToken", req2.tool_invocation_token == "tok123")
    _check("ChatRequest.attempt", req2.attempt == 2)

    # API exports
    _check("api.CancellationTokenSource", api["CancellationTokenSource"] is CancellationTokenSource)
    _check("api.LanguageModelError", api["LanguageModelError"] is LanguageModelError)
    _check("api.ChatLocation", api["ChatLocation"]["Panel"] == 1)
    _check("api.ChatResultFeedbackKind", api["ChatResultFeedbackKind"]["Helpful"] == 1)
    _check("api.ToolMode", api["LanguageModelChatToolMode"]["Auto"] == 1)
    _check("api.ExtensionMode", api["ExtensionMode"]["Production"] == 1)

    # lm.getTools returns list of dicts
    api["lm"]["registerTool"]("test_tool2", {"description": "A test", "schema": {}})
    tools_list = api["lm"]["getTools"]()
    _check("lm.getTools returns dicts", len(tools_list) > 0 and isinstance(tools_list[0], dict))

    # MCP and ignored file APIs exist
    _check("lm.registerMcpServerDefinitionProvider", "registerMcpServerDefinitionProvider" in api["lm"])
    _check("lm.fileIsIgnored", callable(api["lm"]["fileIsIgnored"]))

    # chat context providers
    _check("chat.registerChatWorkspaceContextProvider",
           "registerChatWorkspaceContextProvider" in api["chat"])

    # window.withProgress
    _check("window.withProgress", callable(api["window"]["withProgress"]))
    _check("window.createWebviewPanel", callable(api["window"]["createWebviewPanel"]))


def test_auth() -> None:
    print("── Auth ──")
    import tempfile, shutil
    from ai_editor.auth import AuthService, AuthSession

    tmpdir = tempfile.mkdtemp(prefix="sao_auth_test_")
    try:
        svc = AuthService(storage_dir=tmpdir)

        # No sessions initially
        _check("no sessions", len(svc.list_sessions()) == 0)

        # Create from token
        s = svc.create_session_from_token("github", "ghp_test123", "TestUser")
        _check("session created", s.access_token == "ghp_test123")
        _check("session listed", len(svc.list_sessions("github")) == 1)

        # Get session
        got = svc.get_session("github")
        _check("get_session", got is not None and got.access_token == "ghp_test123")

        # Get session wrong provider
        _check("get_session miss", svc.get_session("bitbucket") is None)

        # Persistence
        svc2 = AuthService(storage_dir=tmpdir)
        _check("persisted", len(svc2.list_sessions("github")) == 1)

        # Remove
        ok = svc.remove_session("github", s.id)
        _check("remove", ok and len(svc.list_sessions("github")) == 0)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_claude_proxy() -> None:
    print("── Claude Proxy ──")
    from ai_editor.claude_proxy import ClaudeProxy

    proxy = ClaudeProxy()
    _check("not running", not proxy.is_running)
    _check("no base_url", proxy.base_url == "")
    _check("has nonce", len(proxy.nonce) == 32)

    port = proxy.start()
    _check("started", proxy.is_running and port > 0)
    _check("base_url", "127.0.0.1" in proxy.base_url)

    env = proxy.get_env()
    _check("env vars", "ANTHROPIC_BASE_URL" in env and "ANTHROPIC_API_KEY" in env)

    # Health check
    import urllib.request
    try:
        r = urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2)
        data = json.loads(r.read())
        _check("health endpoint", data.get("status") == "ok")
    except Exception as e:
        _check("health endpoint", False, str(e))

    proxy.stop()
    _check("stopped", not proxy.is_running)


def test_agents() -> None:
    print("── Agents ──")
    import tempfile, shutil
    from ai_editor.agents import AgentDef, AgentRegistry

    tmpdir = tempfile.mkdtemp(prefix="sao_agent_test_")
    try:
        reg = AgentRegistry()
        builtins = reg.list_all()
        _check(f"builtin agents: {len(builtins)}", len(builtins) >= 5)

        cr = reg.get("code-reviewer")
        _check("code-reviewer exists", cr is not None and cr.builtin)

        _check("get nonexistent", reg.get("nope") is None)

        # Save custom agent
        initial_version = reg._version
        custom = AgentDef(id="test-agent", name="Test Agent",
                          description="For testing", system_prompt="Be helpful.")
        r = reg.save_custom(custom, workspace_root=tmpdir)
        _check("save custom", r.get("ok") is True)
        _check("custom in registry", reg.get("test-agent") is not None)
        saved_version = reg._version
        _check("agent registry version bumps after save",
               saved_version > initial_version)

        # Load custom from disk
        reg2 = AgentRegistry()
        load_initial_version = reg2._version
        reg2.load_custom(workspace_root=tmpdir)
        _check("loaded from disk", reg2.get("test-agent") is not None)
        _check("agent registry version bumps after load",
               reg2._version > load_initial_version)

        # Delete custom
        r = reg.delete_custom("test-agent", workspace_root=tmpdir)
        _check("delete custom", r.get("ok") is True)
        _check("gone after delete", reg.get("test-agent") is None)
        _check("agent registry version bumps after delete",
               reg._version > saved_version)

        # Cannot delete builtin
        r = reg.delete_custom("code-reviewer")
        _check("cant delete builtin", r.get("ok") is False)

        # Prompt section
        section = reg.to_prompt_section()
        _check("prompt section", "Code Reviewer" in section and "invoke_agent" in section)

        # to_dict / from_dict
        d = cr.to_dict()
        _check("to_dict has id", d["id"] == "code-reviewer")
        restored = AgentDef.from_dict(d)
        _check("from_dict roundtrip", restored.id == "code-reviewer")

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_workflows() -> None:
    print("── Workflows ──")
    import tempfile, shutil
    from ai_editor.workflows import WorkflowDef, WorkflowStep, WorkflowRegistry

    tmpdir = tempfile.mkdtemp(prefix="sao_wf_test_")
    try:
        reg = WorkflowRegistry()
        builtins = reg.list_all()
        _check(f"builtin workflows: {len(builtins)}", len(builtins) >= 3)

        rf = reg.get("review-and-fix")
        _check("review-and-fix exists", rf is not None and len(rf.steps) == 2)

        # Save custom workflow
        initial_version = reg._version
        custom = WorkflowDef(
            id="test-wf", name="Test WF", description="Testing",
            steps=[WorkflowStep(prompt="Echo: {{input}}", output_var="out", label="Echo")],
        )
        r = reg.save_custom(custom, workspace_root=tmpdir)
        _check("save custom wf", r.get("ok") is True)
        saved_version = reg._version
        _check("workflow registry version bumps after save",
               saved_version > initial_version)

        # Load from disk
        reg2 = WorkflowRegistry()
        load_initial_version = reg2._version
        reg2.load_custom(workspace_root=tmpdir)
        _check("loaded wf from disk", reg2.get("test-wf") is not None)
        _check("steps preserved", len(reg2.get("test-wf").steps) == 1)
        _check("workflow registry version bumps after load",
               reg2._version > load_initial_version)

        # Delete custom
        r = reg.delete_custom("test-wf", workspace_root=tmpdir)
        _check("delete custom wf", r.get("ok") is True)
        _check("workflow registry version bumps after delete",
               reg._version > saved_version)

        # Cannot delete builtin
        r = reg.delete_custom("review-and-fix")
        _check("cant delete builtin wf", r.get("ok") is False)

        # Prompt section
        section = reg.to_prompt_section()
        _check("wf prompt section", "Review & Fix" in section and "run_workflow" in section)

        # to_dict / from_dict roundtrip
        d = rf.to_dict()
        _check("wf to_dict", d["id"] == "review-and-fix" and len(d["steps"]) == 2)
        restored = WorkflowDef.from_dict(d)
        _check("wf from_dict roundtrip", restored.id == "review-and-fix")

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_tk_window() -> None:
    print("── Tk Window ──")
    from gui_modules.sao_gui_ai_editor import AIEditorPanel

    root = tk.Tk()
    root.withdraw()

    try:
        panel = AIEditorPanel(root, _FakeGui())
        _check("Panel created", True)

        _check("Not visible initially", not panel.is_visible())

        panel.show()
        _check("Show succeeded", panel.is_visible())
        _check("Window exists", panel._win is not None and panel._win.winfo_exists())

        # Verify UI elements
        _check("Chat text", panel._chat_text is not None)
        _check("Input text", panel._input_text is not None)
        _check("Status var", panel._status_var is not None)
        _check("Provider var", panel._provider_var is not None)
        _check("Model var", panel._model_var is not None)
        _check("Sidebar frame", panel._sidebar is not None)
        _check("Sidebar hidden", not panel._sidebar_visible)

        # Slash commands
        commands = {cmd for cmd, _desc in panel._SLASH_COMMANDS}
        _check("Slash commands defined", {"/system", "/eval"}.issubset(commands))

        # Ensure engine init
        panel._ensure_engine()
        _check("Engine initialized", panel._engine is not None)
        _check("Registry initialized", panel._registry is not None)
        _check("Controller initialized", panel._controller is not None)

        # Test slash command execution
        panel._execute_slash_command("/system")
        _check("/system executed", True)

        panel._execute_slash_command("/eval 1 + 1")
        _check("/eval executed", True)

        # Toggle sidebar
        panel._toggle_sidebar()
        _check("Sidebar toggled open", panel._sidebar_visible)
        panel._toggle_sidebar()
        _check("Sidebar toggled closed", not panel._sidebar_visible)

        # Append messages
        panel._render_user_message("Test user message")
        panel._render_assistant_header()
        panel._render_markdown("Hello **bold** and `code` and\n```python\ndef foo():\n    return 42\n```")
        _check("Markdown rendered", True)

        panel._render_tool_call("engine", '{"key": "value"}')
        _check("Tool call rendered", True)

        panel._render_tool_result("engine", '{"ok": true}')
        _check("Tool result rendered", True)

        panel.hide()
        _check("Hide succeeded", not panel.is_visible())

        panel.destroy()
        _check("Destroy succeeded", not panel.is_visible())

    except Exception as e:
        _check(f"Tk window test", False, str(e))
        import traceback
        traceback.print_exc()
    finally:
        try:
            root.destroy()
        except Exception:
            pass


def main() -> None:
    print("=" * 50)
    print("SAO AI Editor — Selftest")
    print("=" * 50)

    test_imports()
    test_tool_registry()
    test_llm_engine()
    test_conversation()
    test_history()
    test_bridge()
    test_app_settings_parity()
    test_phase1_ai_editor_regressions()
    test_instructions()
    test_scopes()
    test_extension_host()
    test_vscode_api()
    test_auth()
    test_claude_proxy()
    test_agents()
    test_workflows()
    test_tk_window()

    print()
    print(f"{'=' * 50}")
    total = _PASS + _FAIL
    if _FAIL == 0:
        print(f"ALL {total} TESTS PASSED ✓")
    else:
        print(f"{_PASS}/{total} passed, {_FAIL} FAILED ✗")
    print(f"{'=' * 50}")

    sys.exit(0 if _FAIL == 0 else 1)


if __name__ == "__main__":
    main()
