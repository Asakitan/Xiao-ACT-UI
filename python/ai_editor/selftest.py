"""AI Editor selftest — verifies all modules, Tk window build, and slash commands.

Run:  python -m ai_editor.selftest
"""

from __future__ import annotations

import json
import shutil
import sys
import os
import tempfile
import threading
import time
import tkinter as tk
from unittest.mock import patch

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


def _wait_until(predicate, timeout: float = 2.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


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


class _FailingSettings(_FakeSettings):
    def save(self):
        raise RuntimeError("disk failed")


class _SettingsGui(_FakeGui):
    def __init__(self, data):
        self.settings = _FakeSettings(data)
        self._ai_engine_actions = dict(_FakeGui._ai_engine_actions)


class _FailingSettingsGui(_FakeGui):
    def __init__(self, data):
        self.settings = _FailingSettings(data)
        self._ai_engine_actions = dict(_FakeGui._ai_engine_actions)


class _FakeVscodeNamespace:
    def __init__(self, html_by_id=None):
        self.html_by_id = dict(html_by_id or {})
        self.delivered = []

    def get_webview_html(self, view_id):
        return self.html_by_id.get(view_id, "")

    def list_webview_view_ids(self):
        return list(self.html_by_id.keys())

    def list_webview_views(self):
        return [
            {
                "view_id": view_id,
                "kind": "webviewView",
                "extension_id": "",
                "title": view_id,
                "html_available": bool(html),
            }
            for view_id, html in self.html_by_id.items()
        ]

    def deliver_webview_message(self, view_id, message):
        if view_id not in self.html_by_id:
            return False
        self.delivered.append((view_id, message))
        return True


class _FakeExtPoints:
    def __init__(self, all_contributions):
        self.all_contributions = all_contributions


class _FakeExtDesc:
    """Minimal extension descriptor for selftest manifest-view lookup."""
    def __init__(self, ext_id, contributes=None, enabled=True, publisher=""):
        self.id = ext_id
        self.contributes = contributes or {}
        self.enabled = enabled
        self.publisher = publisher or ext_id.split(".", 1)[0] if "." in ext_id else ""


class _FakeRegistry:
    def __init__(self, extensions=None):
        self._extensions = list(extensions or [])

    def list_all(self):
        return list(self._extensions)


class _FakeExtensionHost:
    def __init__(self, all_contributions):
        self.ext_points = _FakeExtPoints(all_contributions)
        self.registry = _FakeRegistry(
            self._build_fake_extensions(all_contributions)
        )

    def activate_event(self, event):
        return []

    @staticmethod
    def _build_fake_extensions(contribs):
        """Synthesize _FakeExtDesc entries from all_contributions['views']."""
        views_by_location = contribs.get("views", {})
        if not views_by_location:
            return []
        by_ext: dict = {}
        for location, views in views_by_location.items():
            if not isinstance(views, list):
                continue
            for view in views:
                ext_id = view.get("_extensionId", "unknown.ext")
                if ext_id not in by_ext:
                    by_ext[ext_id] = {}
                by_ext[ext_id].setdefault(location, []).append(view)
        result = []
        for ext_id, loc_views in by_ext.items():
            result.append(_FakeExtDesc(
                ext_id=ext_id,
                contributes={"views": loc_views},
            ))
        return result


def test_app_settings_parity() -> None:
    print("── App Settings Parity ──")
    from ai_editor.app import AIEditorAPI, _activate_window_handle, _normalize_window_geometry

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
        "list_workspace_tree", "open_workspace_file",
        "get_chat_controls", "set_active_provider", "set_active_model",
        "set_active_mode", "set_provider_model", "set_chat_provider",
        "set_chat_controls",
        "get_extension_contributions",
        "list_extension_settings", "get_extension_setting",
        "set_extension_setting", "reset_extension_setting",
        "list_editor_languages", "list_editor_grammars",
        "list_editor_themes", "get_editor_theme",
        "list_extension_activity_bar_items",
        "load_history", "switch_provider", "list_chat_providers",
        "provider_send", "provider_cancel", "provider_new_chat",
        "get_model_info", "test_connection", "set_mode", "save_config",
        "get_mode", "list_models", "save_custom_model",
        "delete_custom_model", "search_extensions",
        "list_installed_extensions", "uninstall_extension",
        "install_extension", "win_minimize", "win_maximize", "win_close",
        "open_text_file", "save_feedback",
    )
    missing = [name for name in js_methods if not callable(getattr(api, name, None))]
    _check("AIEditorAPI JS-callable methods", not missing, ", ".join(missing))

    x, y, w, h = _normalize_window_geometry({"x": 999999, "y": 999999, "w": 999999, "h": 999999})
    _check("saved AI Editor window geometry is clamped to visible screen",
           w >= 600 and h >= 400 and x < 999999 and y < 999999)
    _check("AI Editor foreground activation ignores missing HWND",
           _activate_window_handle(0) is False)
    root_dir = os.path.dirname(os.path.dirname(__file__))
    panels_path = os.path.join(root_dir, "gui_modules", "sao_gui_panels_mixin.py")
    with open(panels_path, "r", encoding="utf-8") as fh:
        panels_src = fh.read()
    _check("AI Editor menu launch stops fisheye overlay",
           "self._stop_fisheye_overlay()" in panels_src)
    app_path = os.path.join(root_dir, "ai_editor", "app.py")
    with open(app_path, "r", encoding="utf-8") as fh:
        app_src = fh.read()
    _check("AI Editor activation does not block webview.start",
           "threading.Thread(target=_activate_ai_editor_window_with_retry" in app_src
           and "webview.start(debug=False)" in app_src)

    data = {
        "ai_editor": {
            "provider": "openai",
            "api_key": "old-key",
            "approval": "bypass",
            "active_chat_provider": "codex",
            "user_instructions": "keep me",
            "provider_keys": {"anthropic": "old-claude"},
            "custom_models": {"kept-model": {"max_input": 123, "max_output": 45}},
            "layout": {"sidebarVisible": False, "editorVisible": True, "chatVisible": False},
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
        "custom_models", "mode", "permissions", "approval", "active_chat_provider",
    )
    _check("load_config advanced fields", all(k in loaded for k in advanced_keys))
    _check("load_config existing safe section",
           loaded.get("claude_code", {}).get("cli_path") == "claude-cli")
    _check("load_config restores UI state payload",
           loaded.get("approval") == "bypass"
           and loaded.get("active_chat_provider") == "codex"
           and loaded.get("layout", {}).get("sidebarVisible") is False
           and loaded.get("layout", {}).get("editorVisible") is True)
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
        "approval": "autopilot",
        "active_chat_provider": "copilot",
        "layout": {"sidebarVisible": True, "editorVisible": True, "chatVisible": True, "panelHeight": "320px"},
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
    _check("approval active provider and layout round-trip",
           loaded.get("approval") == "autopilot"
           and loaded.get("active_chat_provider") == "copilot"
           and loaded.get("layout", {}).get("panelHeight") == "320px")

    from ai_editor import app as app_mod
    from config import SettingsManager
    previous_standalone_settings = getattr(app_mod, "_STANDALONE_SETTINGS", None)
    try:
        with tempfile.TemporaryDirectory() as td:
            standalone_path = os.path.join(td, "settings.json")
            app_mod._STANDALONE_SETTINGS = SettingsManager(standalone_path)
            standalone_api = AIEditorAPI(_FakeGui())
            no_settings_result = standalone_api.save_config({
                "provider": "openai",
                "theme": "light",
                "active_chat_provider": "copilot",
                "layout": {"panelHeight": "280px"},
            })
            switch_result = standalone_api.switch_provider("chat")
            reloaded = AIEditorAPI(_FakeGui()).load_config()
            _check("standalone AI Editor persists settings fallback",
                   no_settings_result.get("ok") is True
                   and not no_settings_result.get("error")
                   and switch_result.get("ok") is True
                   and not switch_result.get("persist_error")
                   and reloaded.get("theme") == "light"
                   and reloaded.get("active_chat_provider") == "chat"
                   and reloaded.get("layout", {}).get("panelHeight") == "280px")
    finally:
        app_mod._STANDALONE_SETTINGS = previous_standalone_settings

    save_failure_result = AIEditorAPI(_FailingSettingsGui({"ai_editor": {}})).save_config({"provider": "openai"})
    _check("save_config surfaces persistence errors",
           save_failure_result.get("error") == "disk failed"
           and save_failure_result.get("applied") is True)

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

    from ai_editor.chat_providers import ChatProviderDef

    test_provider = ChatProviderDef(
        id="test-cli", name="Test CLI", provider_type="openai",
        model="test-model", auto_agent=True)
    provider_gui = _SettingsGui({"ai_editor": {}})
    provider_api = AIEditorAPI(provider_gui)
    provider_api._ensure_engine()
    test_ctrl = provider_api._create_provider_controller(test_provider)
    _check("custom provider creates ChatController",
           hasattr(test_ctrl, "engine")
           and test_ctrl.engine.config.model == "test-model")

    legacy_provider_gui = _SettingsGui({"ai_editor": {
        "_provider_keys": {"openai": "legacy-openai"},
    }})
    legacy_provider_api = AIEditorAPI(legacy_provider_gui)
    legacy_provider_api._ensure_engine()
    legacy_provider = ChatProviderDef(
        id="legacy-openai", name="Legacy OpenAI", provider_type="openai",
        model="legacy-codex")
    legacy_ctrl = legacy_provider_api._create_provider_controller(legacy_provider)
    _check("legacy provider keys applied to provider controllers",
           hasattr(legacy_ctrl, "engine")
           and legacy_ctrl.engine.config.api_key == "legacy-openai"
           and legacy_ctrl.engine.config.model == "legacy-codex")

    wrong_key_gui = _SettingsGui({"ai_editor": {
        "provider": "anthropic",
        "api_key": "active-anthropic",
        "codex": {"transport": "responses"},
    }})
    wrong_key_api = AIEditorAPI(wrong_key_gui)
    wrong_key_api._ensure_engine()
    wrong_key_provider = ChatProviderDef(
        id="wrong-key-openai", name="Wrong Key OpenAI", provider_type="openai")
    wrong_key_ctrl = wrong_key_api._create_provider_controller(wrong_key_provider)
    _check("provider controllers do not reuse another provider key",
           hasattr(wrong_key_ctrl, "engine")
           and wrong_key_ctrl.engine.config.api_key == "")

    proxy_gui = _SettingsGui({"ai_editor": {
        "provider_keys": {"anthropic": "test-anthropic"},
        "claude_code": {"model": "claude-proxy-test"},
    }})
    proxy_api = AIEditorAPI(proxy_gui)
    proxy_api._ensure_engine()
    # claude-code is not built-in; register it explicitly for the proxy test
    proxy_api.register_chat_provider({
        "id": "claude-code",
        "name": "Claude Code",
        "provider_type": "anthropic",
        "api_key": "test-anthropic",
        "model": "claude-proxy-test",
    })
    proxy_result = proxy_api.start_claude_proxy()
    try:
        _check("claude proxy uses Claude Code provider settings",
               proxy_result.get("ok") is True
               and proxy_result.get("model") == "claude-proxy-test"
               and proxy_result.get("env", {}).get("ANTHROPIC_MODEL") == "claude-proxy-test")
    finally:
        proxy_api.stop_claude_proxy()

        stale_fetch_calls = []

        stale_claude_gui = _SettingsGui({"ai_editor": {
            "provider_keys": {"anthropic": "test-anthropic"},
        }})
        stale_claude_api = AIEditorAPI(stale_claude_gui)
        stale_claude_api._ensure_engine()
        def _stale_fetch(provider, base_url, api_key, timeout=10.0):
            stale_fetch_calls.append({
                "provider": provider,
                "base_url": base_url,
                "api_key": api_key,
                "timeout": timeout,
            })
            return {
                "models": [{"id": "official-claude", "name": "Official Claude"}],
                "default_model": "official-claude",
            }
        stale_claude_api._fetch_provider_models = _stale_fetch
        stale_claude = ChatProviderDef(
            id="stale-claude", name="Stale Claude", provider_type="anthropic",
            model="claude-sonnet-4-20250514")
        stale_ctrl = stale_claude_api._create_provider_controller(stale_claude)
        _check("stale Claude default model replaced from official models",
               hasattr(stale_ctrl, "engine")
               and stale_ctrl.engine.config.model == "official-claude"
               and stale_fetch_calls
               and stale_fetch_calls[0].get("base_url") == "https://api.anthropic.com/v1")

    # codex is not built-in; register it as a custom extension-webview provider and verify
    provider_api._ensure_engine()
    provider_api.register_chat_provider({
        "id": "codex",
        "name": "Codex",
        "provider_type": "openai",
        "webview_id": "chatgpt.sidebarView",
    })
    cli_runtime = provider_api.list_chat_providers().get("providers", [])
    cli_codex = next((p for p in cli_runtime if p.get("id") == "codex"), {})
    _check("codex provider is extension WebviewView-backed",
           cli_codex.get("requested_transport") == "webviewView"
           and cli_codex.get("resolved_transport") == "webviewView"
           and cli_codex.get("transport") == "extension-webview"
           and cli_codex.get("runtime_mode") == "extension-webview")

    mode_data = {"ai_editor": {"mode": "ask", "permissions": {"readFile": "allowed"}}}
    mode_api = AIEditorAPI(_SettingsGui(mode_data))
    mode_api._ensure_engine()
    mode = mode_api.get_mode("plan")
    _check("get_mode returns defaults and overrides",
           mode.get("mode") == "ask"
           and mode.get("selected_mode") == "plan"
           and mode.get("defaults", {}).get("runTerminal") == "confirm"
           and mode.get("overrides", {}).get("readFile") == "allowed")

    mcp_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    _check("unsupported MCP transport rejected explicitly",
           "Unsupported MCP transport" in mcp_api.add_mcp_server({
               "id": "http-test",
               "transport": "streamable_http",
               "url": "http://localhost:3000/mcp",
           }).get("error", ""))

    no_window_editor = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    _check("editor mutating API does not fake success without window",
           "not available" in no_window_editor.editor_set_content("x").get("error", "")
           and "not available" in no_window_editor.editor_insert_text("x").get("error", "")
           and "not available" in no_window_editor.editor_go_to_line(1).get("error", "")
           and "not available" in no_window_editor.editor_find_replace("x", "y").get("error", ""))
    _check("open_text_file reports missing window explicitly",
           "No window" in no_window_editor.open_text_file().get("error", ""))

    feedback_gui = _SettingsGui({"ai_editor": {}})
    feedback_api = AIEditorAPI(feedback_gui)
    feedback_result = feedback_api.save_feedback("msg-1", "up")
    _check("save_feedback persists allowed rating",
           feedback_result.get("ok") is True
           and feedback_gui.settings.data.get("ai_editor", {}).get("feedback", {}).get("msg-1", {}).get("rating") == "up")
    _check("save_feedback rejects invalid rating",
           "rating" in feedback_api.save_feedback("msg-1", "maybe").get("error", ""))

    class _FakeWindowChrome:
        def __init__(self) -> None:
            self.x = 100
            self.y = 200
            self.width = 900
            self.height = 700
            self.calls = []

        def move(self, x: int, y: int) -> None:
            self.calls.append(("move", x, y))
            self.x = x
            self.y = y

        def resize(self, width: int, height: int) -> None:
            self.calls.append(("resize", width, height))
            self.width = width
            self.height = height

        def minimize(self) -> None:
            self.calls.append(("minimize",))

        def maximize(self) -> None:
            self.calls.append(("maximize",))

        def restore(self) -> None:
            self.calls.append(("restore",))

    window_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    fake_window = _FakeWindowChrome()
    window_api.set_window(fake_window)
    east_resize = window_api.win_resize_by("e", 50, 0)
    west_clamp = window_api.win_resize_by("w", 500, 0)
    north_resize = window_api.win_resize_by("n", 0, 120)
    south_resize = window_api.win_resize_by("s", 0, 90)
    window_api.win_minimize()
    window_api.win_maximize()
    blocked_resize = window_api.win_resize_by("se", 10, 10)
    window_api.win_maximize()
    _check("window chrome resize clamps and tracks fake pywebview window",
           east_resize.get("ok") is True
           and east_resize.get("width") == 950
           and west_clamp.get("ok") is True
           and west_clamp.get("x") == 450
           and west_clamp.get("width") == 600
           and north_resize.get("ok") is True
           and north_resize.get("y") == 320
           and north_resize.get("height") == 580
           and south_resize.get("ok") is True
           and south_resize.get("height") == 670
           and ("move", 450, 200) in fake_window.calls
           and ("resize", 600, 580) in fake_window.calls
           and ("resize", 600, 670) in fake_window.calls)
    _check("window chrome minimize maximize and resize lock work",
           fake_window.calls[-3:] == [("minimize",), ("maximize",), ("restore",)]
           and blocked_resize.get("ok") is False
           and "maximized" in blocked_resize.get("error", ""))

    with tempfile.TemporaryDirectory() as text_tmp:
        text_path = os.path.join(text_tmp, "picked.py")
        with open(text_path, "w", encoding="utf-8") as fh:
            fh.write("print('picked')\n")

        class _TextDialogWindow:
            def create_file_dialog(self, **kw):
                return [text_path]

        picker_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
        picker_api.set_window(_TextDialogWindow())
        picked = picker_api.open_text_file()
        _check("open_text_file reads selected text file",
               picked.get("ok") is True
               and picked.get("language") == "python"
               and "print('picked')" in picked.get("content", ""))

    unsupported_tool_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    unsupported_tool_api._ensure_engine()
    unsupported_tool_api._vscode_ns._register_tool_definition(
        "schema_only_api", {"type": "object"})
    unsupported_tool = unsupported_tool_api.invoke_lm_tool("schema_only_api", {})
    _check("API invoke_lm_tool reports schema-only unsupported as error",
           unsupported_tool.get("unsupported") is True
           and unsupported_tool.get("needsExtensionRuntime") is True
           and "error" in unsupported_tool
           and unsupported_tool.get("ok") is not True)

    test_cfg = mcp_api.test_connection
    _check("test_connection API accepts advanced params",
           callable(test_cfg))

    agent_api = AIEditorAPI(_SettingsGui({"ai_editor": {"mode": "agent"}}))
    agent_api._ensure_engine()
    agent_api._set_active_agent("code-reviewer")
    disabled = agent_api._controller._disabled_tools
    _check("agent tool allowlist disables excluded tools",
           "runTerminal" in disabled and "readFile" not in disabled)
    agent_api.clear_active_agent()

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

    mcp_policy_api = AIEditorAPI(_SettingsGui({"ai_editor": {
        "mode": "agent",
        "mcp": {"access": "read_only"},
    }}))
    mcp_policy_api._ensure_engine()
    mcp_policy_api.register_mcp_tools(
        "selftest_tools",
        [
            {"name": "read_info", "description": "Read info", "inputSchema": {}},
            {"name": "write_info", "description": "Write info", "inputSchema": {}},
        ],
        {
            "read_info": lambda **kw: {"ok": True, "read": True},
            "write_info": lambda **kw: {"ok": True, "write": True},
        },
    )
    mcp_tool_names = [
        item.get("function", {}).get("name")
        for item in (mcp_policy_api._controller.extra_tools or [])
    ]
    mcp_list_items = {t.get("name"): t for t in mcp_policy_api.list_tools().get("tools", [])}
    _check("MCP read_only exposes only read-like controller tools",
           "mcp_selftest_tools_read_info" in mcp_tool_names
           and "mcp_selftest_tools_write_info" not in mcp_tool_names)
    _check("MCP tool list includes UI metadata and normalized schema",
           mcp_list_items.get("mcp_selftest_tools_read_info", {}).get("serverId") == "selftest_tools"
           and mcp_list_items.get("mcp_selftest_tools_read_info", {}).get("sourceName") == "read_info"
           and mcp_list_items.get("mcp_selftest_tools_read_info", {}).get("parameters", {}).get("type") == "object"
           and mcp_list_items.get("mcp_selftest_tools_read_info", {}).get("requires_confirm") is False)
    bad_mcp_args = mcp_policy_api._controller.mcp_dispatch(
        "mcp_selftest_tools_read_info", "[]") if mcp_policy_api._controller.mcp_dispatch else "{}"
    _check("MCP dispatch rejects non-object arguments explicitly",
           "must be a JSON object" in json.loads(bad_mcp_args).get("error", ""))

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
        "approval": "default",
        "active_chat_provider": "chat",
        "layout": {"sidebarVisible": False},
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
            and controls.get("approval") == "default"
            and controls.get("active_chat_provider") == "chat"
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
           and controls_api._engine.config.model == ""
           and stored_controls.get("provider") == "anthropic"
           and stored_controls.get("model") == ""
           and stored_controls.get("provider_keys", {}).get("anthropic") == "old-claude"
           and stored_controls.get("future_section") == {"enabled": True})

    no_key_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    missing_key_models = no_key_api.list_provider_models(
        "anthropic", "https://api.anthropic.com/v1", "")
    _check("anthropic official model list requires API key",
           missing_key_models.get("default_model") == ""
           and "API key" in missing_key_models.get("error", ""))

    missing_openai_models = no_key_api.list_provider_models(
        "openai", "https://api.openai.com/v1", "")
    _check("openai official model list requires API key",
           missing_openai_models.get("default_model") == ""
           and "API key" in missing_openai_models.get("error", ""))

    with tempfile.TemporaryDirectory() as tmpdir:
        os.makedirs(os.path.join(tmpdir, "src"), exist_ok=True)
        os.makedirs(os.path.join(tmpdir, "node_modules"), exist_ok=True)
        os.makedirs(os.path.join(tmpdir, ".git"), exist_ok=True)
        sample_file = os.path.join(tmpdir, "src", "sample.py")
        with open(sample_file, "w", encoding="utf-8") as fh:
            fh.write("print('tree')\n")
        with open(os.path.join(tmpdir, "README.md"), "w", encoding="utf-8") as fh:
            fh.write("# hi\n")
        tree_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
        tree_api._workspace_root = lambda: tmpdir
        tree = tree_api.list_workspace_tree()
        root_names = {entry.get("name") for entry in tree.get("entries", [])}
        _check("workspace tree filters ignored directories",
               "src" in root_names and "README.md" in root_names
               and "node_modules" not in root_names and ".git" not in root_names)
        _check("workspace tree keeps directories before files",
               tree.get("entries", [{}])[0].get("type") == "directory"
               and tree.get("entries", [{}])[0].get("name") == "src")
        opened = tree_api.open_workspace_file("src/sample.py")
        _check("open_workspace_file returns content and language",
               opened.get("path") == "src/sample.py"
               and opened.get("absolute_path") == sample_file
               and opened.get("language") == "python"
               and "print('tree')" in opened.get("content", ""))
        blocked_path = tree_api.open_workspace_file("../outside.py")
        _check("workspace file API blocks path traversal",
               "escapes workspace" in blocked_path.get("error", ""))
        outside_dir = tempfile.mkdtemp()
        try:
            outside_file = os.path.join(outside_dir, "outside.txt")
            with open(outside_file, "w", encoding="utf-8") as fh:
                fh.write("outside")
            _check("workspace path safety rejects outside real paths",
                   tree_api._is_workspace_safe_path(tmpdir, outside_file) is False)

            link_path = os.path.join(tmpdir, "outside-link.txt")
            try:
                os.symlink(outside_file, link_path)
            except (OSError, NotImplementedError):
                link_path = ""
            if link_path:
                tree = tree_api.list_workspace_tree()
                root_names = {entry.get("name") for entry in tree.get("entries", [])}
                _check("workspace tree hides escaping symlinks",
                       "outside-link.txt" not in root_names)
                blocked_link = tree_api.open_workspace_file("outside-link.txt")
                _check("workspace file API blocks escaping symlinks",
                       "escapes workspace" in blocked_link.get("error", ""))
        finally:
            shutil.rmtree(outside_dir, ignore_errors=True)

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
            and chat_provider_result.get("controls", {}).get("active_chat_provider") == "chat"
            and controls_gui.settings.data["ai_editor"].get("active_chat_provider") == "chat")

    aggregate_result = controls_api.set_chat_controls({
        "provider": "openai", "model": "gpt-4o-mini", "mode": "plan",
         "agent_id": "code-reviewer", "active_chat_provider": "chat", "approval": "autopilot",
    })
    _check("set_chat_controls aggregate setter updates state",
           aggregate_result.get("ok") is True
           and aggregate_result.get("controls", {}).get("provider") == "openai"
           and aggregate_result.get("controls", {}).get("model") == "gpt-4o-mini"
           and aggregate_result.get("controls", {}).get("mode") == "plan"
            and aggregate_result.get("controls", {}).get("active_agent_id") == "code-reviewer"
            and aggregate_result.get("controls", {}).get("approval") == "autopilot"
            and controls_gui.settings.data["ai_editor"].get("approval") == "autopilot")

    provider_model_result = controls_api.set_provider_model("anthropic", "claude-test")
    _check("set_provider_model compatibility alias updates provider and model",
           provider_model_result.get("ok") is True
           and provider_model_result.get("controls", {}).get("provider") == "anthropic"
           and provider_model_result.get("controls", {}).get("model") == "claude-test")


def test_phase1_ai_editor_regressions() -> None:
    print("── Phase 1 AI Editor Regressions ──")
    from ai_editor.app import AIEditorAPI
    from ai_editor.chat_providers import ChatProviderRegistry, get_provider_registry
    from ai_editor.extension_host import ExtensionDescription
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

    # Only "chat" is built-in now; copilot/claude-code/codex are extension-registered.
    # flags() tests the built-in registry only.
    def flags(settings):
        getter = lambda key, default=None: settings.get(key, default)
        return {p["id"]: p["available"]
                for p in ChatProviderRegistry().list_available(getter)}

    cli_only_settings = {"ai_editor": {
        "claude_code": {"cli_path": sys.executable},
        "codex": {"cli_path": sys.executable},
        "provider_keys": {"openai": "test-openai"},
    }}
    registry_cli_flags = flags(cli_only_settings)
    _check("native and extension providers are not hidden by CLI availability",
           registry_cli_flags.get("chat") is True)

    api_cli = AIEditorAPI(_SettingsGui(cli_only_settings))
    # Register the extension-backed providers so downstream tests can use them
    api_cli.register_chat_provider({
        "id": "copilot", "name": "Copilot",
        "provider_type": "github-copilot",
        "webview_id": "",
        "metadata": {
            "native_chat": True,
            "capability": "github-copilot-chat",
            "requested_transport": "chatParticipant",
            "resolved_transport": "native-chat",
            "transport": "native-chat",
            "runtime_mode": "native-chat",
        },
    })
    api_cli.register_chat_provider({
        "id": "claude-code", "name": "Claude Code",
        "provider_type": "anthropic",
        "webview_id": "anthropic.claude-code",
    })
    api_cli.register_chat_provider({
        "id": "codex", "name": "Codex",
        "provider_type": "openai",
        "webview_id": "chatgpt.sidebarView",
    })
    api_cli_items = {p["id"]: p
                     for p in api_cli.list_chat_providers().get("providers", [])}
    api_cli_flags = {k: v.get("available") for k, v in api_cli_items.items()}
    api_cli_provider_rows = api_cli.list_chat_providers().get("providers", [])
    # Only "chat" is a built-in; one built-in name check
    ordered_builtin_rows = [p for p in api_cli_provider_rows if p.get("builtin")]
    ordered_builtin_names = [p.get("name") for p in ordered_builtin_rows]
    _check("built-in chat provider names are exact",
           ordered_builtin_names == ["Assistant"])
    _check(
        "Copilot chat provider is native ChatWidget participant surface",
        api_cli_items.get("copilot", {}).get("provider_type") == "github-copilot",
    )
    copilot_switch = api_cli.switch_provider("copilot")
    _check("switch_provider selects native Copilot surface without CLI controller",
           copilot_switch.get("ok") is True
           and copilot_switch.get("provider") == "copilot"
           and api_cli.get_chat_controls().get("active_chat_provider") == "copilot"
           and "copilot" not in api_cli._provider_controllers)

    class _FakeProviderController:
        def __init__(self) -> None:
            self.is_running = False
            self.sent = []
            self.new_chat_calls = 0

        def send(self, message: str, agent_mode: bool = False) -> None:
            self.sent.append((message, agent_mode))

        def cancel(self) -> None:
            self.is_running = False

        def new_conversation(self, system_prompt: str = "") -> None:
            self.new_chat_calls += 1

    copilot_new = api_cli.provider_new_chat("copilot")
    _check("provider_new_chat resets native Copilot surface without provider controller",
           copilot_new.get("ok") is True
           and copilot_new.get("provider") == "copilot"
           and "copilot" not in api_cli._provider_controllers)
    _check("provider_cancel rejects unknown provider ids explicitly",
           "Unknown provider" in api_cli.provider_cancel("missing-provider").get("error", ""))
    # Only the "chat" built-in cannot be overridden/unregistered; copilot is now custom
    _check("built-in provider surfaces cannot be overridden or unregistered",
           "built-in provider" in api_cli.register_chat_provider({
               "id": "chat", "name": "Override", "provider_type": "openai"
           }).get("error", "")
           and "built-in provider" in api_cli.unregister_chat_provider("chat").get("error", ""))

    plugin_provider_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    plugin_provider_api.register_chat_provider({
        "id": "plugin-demo",
        "name": "Plugin Demo",
        "provider_type": "openai",
        "api_key": "local-test-key",
        "model": "plugin-model",
    })
    plugin_new_chat = plugin_provider_api.provider_new_chat("plugin-demo")
    plugin_new_chat_again = plugin_provider_api.provider_new_chat("plugin-demo")
    _check("provider_new_chat creates provider controller on first use",
           plugin_new_chat.get("ok") is True
           and plugin_new_chat.get("created_controller") is True
           and "plugin-demo" in plugin_provider_api._provider_controllers
           and plugin_new_chat_again.get("created_controller") is False)
    _check("provider_send rejects empty provider message payloads",
           plugin_provider_api.provider_send("plugin-demo", "   ").get("error") == "Empty message")
    _check("unregister_chat_provider removes custom providers only",
           plugin_provider_api.unregister_chat_provider("plugin-demo").get("ok") is True
           and plugin_provider_api._provider_registry.get("plugin-demo") is None)
    _check("list_chat_providers shows native/webview metadata",
           api_cli_items.get("claude-code", {}).get("runtime_mode") == "extension-webview"
           and api_cli_items.get("codex", {}).get("runtime_mode") == "extension-webview"
           and api_cli_items.get("claude-code", {}).get("transport") == "extension-webview"
           and api_cli_items.get("codex", {}).get("transport") == "extension-webview"
           and "cli_available" in api_cli_items.get("codex", {}))
    _check("extension providers expose CLI status without CLI gating",
           api_cli_items.get("claude-code", {}).get("available") is True
           and api_cli_items.get("codex", {}).get("available") is True)

    # Build a fresh registry with codex/claude-code registered to test extension transport
    api_key_settings = {"ai_editor": {
        "codex": {"model": "codex-status-test"},
        "provider_keys": {"anthropic": "test-anthropic", "openai": "test-openai"},
    }}
    from ai_editor.chat_providers import describe_provider_status, ChatProviderDef
    _codex_ext = ChatProviderDef(id="codex", name="Codex", provider_type="openai",
                                  webview_id="chatgpt.sidebarView")
    _claude_code_ext = ChatProviderDef(id="claude-code", name="Claude Code",
                                        provider_type="anthropic",
                                        webview_id="anthropic.claude-code")
    getter_key = lambda key, default=None: api_key_settings.get(key, default)
    _check("extension providers are not CLI-gated even with API keys",
           describe_provider_status(_codex_ext, getter_key).get("available") is True
           and describe_provider_status(_claude_code_ext, getter_key).get("available") is True)
    _check("extension provider transport is WebviewView",
           describe_provider_status(_codex_ext, getter_key).get("requested_transport") == "webviewView"
           and describe_provider_status(_codex_ext, getter_key).get("resolved_transport") == "webviewView"
           and describe_provider_status(_codex_ext, getter_key).get("transport") == "extension-webview"
           and describe_provider_status(_codex_ext, getter_key).get("runtime_mode") == "extension-webview")

    html_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "web", "ai_editor_app.html")
    with open(html_path, "r", encoding="utf-8") as fh:
        html = fh.read()
    _check("right-sidebar tabs use exact built-in labels",
           "tab_chat:'Assistant'" in html
           and "provider_copilot_label:'Copilot'" in html
           and "provider_claude_code_label:'Claude Code'" in html
           and "provider_codex_label:'Codex'" in html)
    _check("frontend does not synthesize fake built-in providers",
           "SYNTHETIC_PROVIDERS" not in html)
    _check("frontend ships frameless resize handles and bridge call",
           html.count('data-resize-edge="') == 8
           and "querySelectorAll('[data-resize-edge]')" in html
            and "call('win_resize_by',state.edge,dx,dy)" in html
            and "titlebar.addEventListener('pointerdown'" in html
            and "getTitlebarResizeEdge" in html)
    _check("frontend forces editor pane visible after opening",
           "function _ensureEditorPaneVisible()" in html
           and "if(content.getBoundingClientRect().width<120)" in html
           and "_ensureEditorPaneVisible();" in html)
    _check("frontend window close plays exit animation before backend close",
           "body.window-closing" in html
           and "function requestWindowClose()" in html
           and "setTimeout(()=>" in html
           and "window.requestWindowClose=requestWindowClose" in html)
    _check("provider webviews bridge persistent vscode state",
           'type:"webview-set-state"' in html
           and 'webview_set_state' in html
           and 'getState:function(){return _state}' in html)
    _check("frontend accepts provider webview pushes",
           "const providerId=String(viewId).startsWith('provider.')?String(viewId).slice(9):''" in html
            and "isNativeCliProvider" not in html
           and "document.querySelector('.right-panel[data-view-id=\"'+viewSelector+'\"]')" in html)
    _check("extension provider panels request runtime webviews first",
            "async function mountProviderWebviewPanel(panel,p)" in html
            and "call('get_provider_webview',requestId)" in html
            and "_injectWebviewHtml(panel,viewId,wv.html,wv.state)" in html
            and "renderProviderWebviewPlaceholder(panel,p,'missing-runtime')" in html)
    _check("Copilot provider renders as native tab, not extension webview",
            "if(p.id==='copilot')return;\n    if(p.available===false" not in html
            and "EXTENSION_WEBVIEW_PROVIDER_IDS" not in html
            and "panel.dataset.providerSurface=isExtensionWebviewProvider(p)?'extension-webview':'native-chat'" in html
            and "const BUILTIN_PROVIDER_ORDER=[];" in html)
    _check("chat history entry moved into Chat controls",
            'data-panel="history"' not in html
            and html.count('onclick="openChatHistory()"') >= 2
            and "async function openChatHistory()" in html
            and "window.openChatHistory=openChatHistory" in html
            and "else if(cmd==='/history')openChatHistory();" in html
            and "{label:'Chat History',shortcut:'',action:()=>openChatHistory()}" in html)
    _check("frontend supports dynamic extension webview provider tabs",
            "function isExtensionWebviewProvider(providerOrId)" in html
            and "dynamicExtensionProviderDefs" in html
            and "provider_tabs_changed'||event==='extension_views_changed" in html
            and "ensureExtensionProviderPanel(data,viewId)" in html
            and "rememberDynamicProvidersFromEventData(data)" in html)
    _check("frontend loads dynamic editor language contributions",
            "call('list_editor_languages')" in html
            and "call('list_editor_themes')" in html
            and "call('get_editor_theme'" in html
            and "function applyEditorLanguages(rows)" in html
            and "function loadEditorThemes()" in html
            and "function renderEditorColorThemeOptions()" in html
            and "function applyEditorColorTheme(value,options)" in html
            and "THEME_COLOR_VAR_MAP" in html
            and "EXTENSION_EDITOR_THEMES" in html
            and "grammarScopes" in html
            and "badges.push('TextMate')" in html
            and "LANGUAGE_BY_EXT" in html
            and "languageForFileName(name)" in html)
    _check("frontend renders extension settings modified reset controls",
            "function renderExtensionSettings()" in html
            and "reset_extension_setting" in html
            and "markExtensionSettingRow" in html
            and "function applyExtensionSettingsFilter()" in html
            and "ext-settings-category" in html
            and "dataset.extSettingsCategory" in html
            and "@modified" in html
            and "ext-setting-badge" in html
            and "Invalid JSON" in html)
    _check("frontend renders extension activity bar views dynamically",
            "function renderExtensionContainerContent(item)" in html
            and "function renderExtensionTreeView(view)" in html
            and "function renderExtensionTreeNode(viewId,node,depth,viewVersion)" in html
            and "function extensionTreeThemeIconGlyph(id)" in html
            and "function extensionTreeIconGlyph(node,collapsible)" in html
            and "function extensionTreeItemTitle(node)" in html
            and "wrap.dataset.contextValue=String(node.contextValue)" in html
            and "wrap.dataset.resourceUri=String(node.resourceUri)" in html
            and "node.accessibilityInformation||{}" in html
            and "node.themeIcon?node.themeIcon:(node?node.iconPath:null)" in html
            and "checkbox.isChecked?'☑':'☐'" in html
            and "className='ext-tree-icon'" in html
            and "tree.setAttribute('role','tree')" in html
            and "event==='extension_tree_changed'" in html
            and "function scheduleExtensionActivityRefresh()" in html
            and "function appendExtensionTitleActions(title,view,state)" in html
            and "function showExtensionActionMenu(x,y,actions,runner)" in html
            and "let _extTreeChildRequestSeq=0" in html
            and "function loadExtensionTreeChildren(viewId,node,childBox,depth,viewVersion)" in html
            and "function setExtensionTreeStatus(childBox,depth,text,kind)" in html
            and "node._childRequestId!==requestId||!childBox.isConnected" in html
            and "expectedVersion!==responseVersion" in html
            and "function focusRevealedExtensionTree(tree)" in html
            and ".ext-tree-node[data-revealed=\"1\"]>.sb-item" in html
            and "target.scrollIntoView({block:'nearest',inline:'nearest'})" in html
            and "call('load_extension_tree_children',viewId,node.handle)" in html
            and "call('set_extension_tree_item_expanded',viewId,node.handle,!!expanded)" in html
            and "call('select_extension_tree_item',viewId,node.handle)" in html
            and "call('execute_extension_tree_item_action',viewId,node.handle,action.command)" in html
            and "window.pywebview.api.execute_command(node.command.command" in html
            and "function renderExtensionWebviewView(view)" in html
            and "_injectWebviewHtml(host,view.id||state.view_id||state.viewId,html,state.state)" in html
            and "renderExtensionContainerContent(item)" in html)
    _check("webview bridge preserves raw and falsy messages",
            "postExtensionMessageToWebview(iframe,msg)" in html
            and "iframe.contentWindow.postMessage(msg,'*')" in html
            and "if(!viewId||!tok||_webviewTokens[viewId]!==tok)return;" in html
            and "if(!viewId||!msg||!tok" not in html)
    _check("non-webview provider panels use backend provider callbacks",
           "function _buildChatProviderPanel(panel,p)" in html
           and "send.onclick=()=>providerSend(pid)" in html
           and "stop.onclick=()=>providerCancel(pid)" in html)
    _check("webview bridge avoids raw script parser sentinel",
           "const bridgeScript='<script>'" not in html
           and "const bridgeScript='<scr'+'ipt>'" in html)
    _check("frontend Explorer calls workspace tree and file APIs",
           "call('list_workspace_tree',relPath||'')" in html
           and "call('list_workspace_tree',entry.path||'')" in html
           and "openWorkspaceFile(entry.path)" in html
           and "call('open_workspace_file',relPath||'')" in html)
    _check("inline HTML handlers are exported to window",
           all(token in html for token in (
               "window.refreshExplorer=refreshExplorer",
               "window.refreshPremiumGuide=refreshPremiumGuide",
               "window.closeFindBar=closeFindBar",
               "window.doReplace=doReplace",
               "window.doReplaceAll=doReplaceAll",
               "window.toggleExtInstall=toggleExtInstall",
           )))

    state_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    _check("provider webview state survives backend lookup without vscode namespace",
           state_api.webview_set_state("provider.codex", {"draft": 1}).get("ok") is True
           and state_api.get_provider_webview("codex").get("state") == {"draft": 1})

    app_path = os.path.join(os.path.dirname(__file__), "app.py")
    with open(app_path, "r", encoding="utf-8") as fh:
        app_source = fh.read()
    _check("provider surfaces do not import provider_runtime_views shim",
           "provider_runtime_views" not in app_source
           and "CliProviderWebviewRuntime" not in app_source
           and "_register_cli_provider_view(" not in app_source)

    runtime_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    runtime_api._controller = object()
    runtime_api._provider_registry = get_provider_registry()
    runtime_ns = _FakeVscodeNamespace({"chatgpt.sidebarView": "<html>runtime codex</html>"})
    runtime_api._vscode_ns = runtime_ns
    runtime_result = runtime_api.get_provider_webview("codex")
    post_result = runtime_api.webview_post_message("chatgpt.sidebarView", {"type": "send", "text": "hi"})
    _check("provider runtime webview HTML wins for Codex",
            runtime_result.get("source") == "runtime"
            and runtime_result.get("view_id") == "chatgpt.sidebarView"
            and "runtime codex" in runtime_result.get("html", ""))
    _check("webview postMessage uses vscode namespace bridge",
           post_result.get("ok") is True
            and runtime_ns.delivered == [("chatgpt.sidebarView", {"type": "send", "text": "hi"})])

    manifest_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    manifest_api._controller = object()
    manifest_api._provider_registry = get_provider_registry()
    manifest_api._vscode_ns = _FakeVscodeNamespace({})
    manifest_api._ext_host = _FakeExtensionHost({
        "views": {
            "codex": [{
                "id": "chatgpt.sidebarView",
                "name": "Codex",
                "_extensionId": "openai.chatgpt",
                "_viewLocation": "codex",
            }]
        },
        "commands": [{"command": "chatgpt.newChat", "_extensionId": "openai.chatgpt"}],
        "chatParticipants": [],
        "languageModelTools": [],
        "authentication": [],
        "mcpServerDefinitionProviders": [],
    })
    manifest_result = manifest_api.get_provider_webview("codex")
    _check("manifest-only provider exposes declared view without fake HTML",
           manifest_result.get("available") is False
            and manifest_result.get("source") == "manifest"
            and manifest_result.get("view_id") == "chatgpt.sidebarView"
            and manifest_result.get("html") == "")

    dynamic_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    dynamic_api._ensure_engine()
    dynamic_events = []
    dynamic_api._emit = lambda event, data: dynamic_events.append((event, data))
    dynamic_desc = ExtensionDescription.from_package_json({
        "name": "dynamic-view",
        "publisher": "selftest",
        "version": "0.0.1",
        "activationEvents": ["onView:selftest.dynamic.view"],
        "contributes": {
            "views": {"copilot": [{
                "id": "selftest.dynamic.view",
                "name": "Selftest Dynamic View",
                "type": "webview",
            }]},
        },
    }, "/tmp/selftest-dynamic-view")
    dynamic_api._ext_host.registry.register(dynamic_desc)
    dynamic_api._ext_host.ext_points.process(dynamic_desc)
    dynamic_providers = {
        p.get("id"): p for p in dynamic_api.list_chat_providers().get("providers", [])
    }
    dynamic_row = dynamic_providers.get("selftest.dynamic.view", {})
    _check("manifest-only webview does NOT create phantom provider tab",
           not dynamic_row)

    class _DynamicWebviewProvider:
        def __init__(self, html_text):
            self.html_text = html_text

        def resolveWebviewView(self, view, context=None, token=None):
            view.webview.html = self.html_text

    dynamic_api._vscode_ns.build(dynamic_desc)["window"]["registerWebviewViewProvider"](
        "selftest.dynamic.view", _DynamicWebviewProvider("<div>dynamic-runtime</div>"))
    dynamic_result = dynamic_api.get_provider_webview("selftest.dynamic.view")
    _check("runtime registerWebviewViewProvider feeds dynamic provider HTML",
           dynamic_result.get("source") == "runtime"
           and dynamic_result.get("view_id") == "selftest.dynamic.view"
           and "dynamic-runtime" in dynamic_result.get("html", "")
           and any(event == "provider_tabs_changed" for event, _ in dynamic_events))

    lazy_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    lazy_api._ensure_engine()
    lazy_desc = ExtensionDescription.from_package_json({
        "name": "lazy-view",
        "publisher": "selftest",
        "version": "0.0.1",
        "activationEvents": ["onView:selftest.lazy.view"],
        "contributes": {
            "views": {"copilot": [{
                "id": "selftest.lazy.view",
                "name": "Selftest Lazy View",
                "type": "webview",
            }]},
        },
    }, "/tmp/selftest-lazy-view")
    lazy_api._ext_host.registry.register(lazy_desc)
    lazy_api._ext_host.ext_points.process(lazy_desc)
    activated_events = []
    real_activate_event = lazy_api._ext_host.activate_event

    def _activate_lazy(event):
        activated_events.append(event)
        if event == "onView:selftest.lazy.view":
            lazy_api._vscode_ns.build(lazy_desc)["window"]["registerWebviewViewProvider"](
                "selftest.lazy.view", _DynamicWebviewProvider("<div>lazy-runtime</div>"))
            return [object()]
        return real_activate_event(event)

    lazy_api._ext_host.activate_event = _activate_lazy
    lazy_result = lazy_api.get_provider_webview("selftest.lazy.view")
    _check("get_provider_webview activates onView before reading runtime HTML",
           activated_events == ["onView:selftest.lazy.view"]
           and lazy_result.get("source") == "runtime"
           and "lazy-runtime" in lazy_result.get("html", ""))

    webview_event_api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
    webview_event_api._ensure_engine()
    webview_events = []
    webview_event_api._emit = lambda event, data: webview_events.append((event, data))
    webview_event_api._vscode_ns.build(dynamic_desc)["window"]["registerWebviewViewProvider"](
        "selftest.message.view", _DynamicWebviewProvider("<div>message-runtime</div>"))
    view = webview_event_api._vscode_ns._webview_views.get("selftest.message.view")
    posted_zero = view.webview.postMessage(0) if view is not None else False
    message_event = next((data for event, data in webview_events if event == "webview_message"), {})
    _check("extension webview postMessage emits raw falsy payload",
           posted_zero is True
           and message_event.get("view_id") == "selftest.message.view"
           and message_event.get("message") == 0
           and message_event.get("raw_message") == 0
           and message_event.get("rawMessage") == 0
           and message_event.get("data") == 0)

    ext_defaults = AIEditorAPI(_SettingsGui({"ai_editor": {"extensions": {"enabled_contributions": ["commands"]}}}))
    _check("extension views contribution remains enabled for old settings",
           "views" in ext_defaults._enabled_extension_contributions())


def test_tool_registry() -> None:
    print("── Tool Registry ──")
    from ai_editor.tool_registry import ToolRegistry
    from ai_editor.engine_tools import register_engine_tools
    from ai_editor.vscode_api import LanguageModelToolResult

    reg = ToolRegistry()
    register_engine_tools(reg, _FakeGui())

    tools = reg.list_tools()
    _check(f"tools registered: {len(tools)}", len(tools) >= 10)

    cats = reg.categories()
    _check(f"categories: {cats}", len(cats) >= 4)

    schemas = reg.to_openai_tools()
    _check(f"OpenAI schemas: {len(schemas)}", len(schemas) == len(tools))

    reg.register("emptySchema", "Empty schema", {}, lambda **kw: {"ok": True})
    empty_schema = reg.get("emptySchema").to_openai_schema()["function"]["parameters"]
    _check("empty tool schema normalized for LLM tools",
           empty_schema.get("type") == "object"
           and isinstance(empty_schema.get("properties"), dict))

    direct_desc = reg.get("emptySchema")
    _check("tool descriptor input_schema stays normalized",
           direct_desc is not None
           and direct_desc.input_schema.get("type") == "object"
           and isinstance(direct_desc.input_schema.get("properties"), dict))

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

    result = reg.execute("engine", json.dumps({"action": "settings_get", "key": "missing"}))
    data = json.loads(result)
    _check("engine(settings_get) no settings is explicit error",
           "Settings not available" in data.get("error", ""))

    result = reg.execute("readFile", "")
    data = json.loads(result)
    _check("missing required tool args are explicit",
           "Missing required argument" in data.get("error", "")
           and "path" in data.get("error", ""))

    result = reg.execute("readFile", "[]")
    data = json.loads(result)
    _check("object-schema tool rejects array args",
           "must be a JSON object" in data.get("error", ""))

    editor_content = json.loads(reg.execute("editor_getContent", "{}"))
    editor_selection = json.loads(reg.execute("editor_getSelection", "{}"))
    _check("editor read tools report missing API explicitly",
           "No editor API available" in editor_content.get("error", "")
           and "No editor API available" in editor_selection.get("error", ""))

    class _FakeEditorApi:
        def __init__(self):
            self._window = object()
            self.calls = []

        def editor_get_content(self):
            return {"content": "print('hi')", "language": "python"}

        def editor_set_content(self, content, language=""):
            self.calls.append((content, language))
            return {"ok": True, "length": len(content), "language": language}

        def editor_get_selection(self):
            return {"selection": "hi", "start": 7, "end": 9}

    reg_editor = ToolRegistry()
    fake_editor_api = _FakeEditorApi()
    register_engine_tools(reg_editor, _FakeGui(), fake_editor_api)
    editor_set = json.loads(reg_editor.execute(
        "editor_setContent", json.dumps({"content": "abc", "language": "text"})))
    editor_get = json.loads(reg_editor.execute("editor_getContent", "{}"))
    _check("editor tools call live editor API",
           editor_set.get("ok") is True
           and fake_editor_api.calls == [("abc", "text")]
           and editor_get.get("content") == "print('hi')")

    reg_todo = ToolRegistry()
    todo_gui = _SettingsGui({})
    register_engine_tools(reg_todo, todo_gui)
    todo_add = json.loads(reg_todo.execute(
        "manageTodoList", json.dumps({"action": "add", "text": "first"})))
    todo_update = json.loads(reg_todo.execute(
        "manageTodoList", json.dumps({"action": "update", "index": 0, "text": "done"})))
    todo_list = json.loads(reg_todo.execute(
        "manageTodoList", json.dumps({"action": "list"})))
    todo_remove = json.loads(reg_todo.execute(
        "manageTodoList", json.dumps({"action": "remove", "index": 0})))
    _check("todo tool add/update/list/remove is runnable",
           todo_add.get("ok") is True
           and todo_update.get("new") == "done"
           and todo_list.get("items", [{}])[0].get("text") == "done"
           and todo_remove.get("removed") == "done"
           and todo_remove.get("count") == 0)

    settings_gui = _SettingsGui({"ai_editor": {"nested": {"mode": "plan"}}})
    reg_settings = ToolRegistry()
    register_engine_tools(reg_settings, settings_gui)
    settings_set = json.loads(reg_settings.execute(
        "engine", json.dumps({
            "action": "settings_set",
            "key": "ai_editor.nested.mode",
            "value": "agent",
        })))
    settings_get = json.loads(reg_settings.execute(
        "engine", json.dumps({
            "action": "settings_get",
            "key": "ai_editor.nested.mode",
        })))
    _check("engine settings tools support nested keys",
           settings_set.get("ok") is True
           and settings_set.get("previous") == "plan"
           and settings_gui.settings.saved is True
           and settings_get.get("value") == "agent"
           and settings_get.get("found") is True)

    reg.register(
        "payloadEcho",
        "Echo payload dict",
        {"type": "object", "properties": {"value": {"type": "string"}},
         "required": ["value"]},
        lambda payload: {"echo": payload.get("value")},
    )
    payload_echo = json.loads(reg.execute("payloadEcho", '{"value":"pong"}'))
    _check("single-arg dict handler supported",
           payload_echo.get("echo") == "pong")

    reg.register(
        "lmResult",
        "Return LM result object",
        {"type": "object", "properties": {}},
        lambda: LanguageModelToolResult.text("done"),
    )
    lm_result = json.loads(reg.execute("lmResult", "{}"))
    _check("LM tool result normalized to JSON",
           lm_result.get("content", [{}])[0].get("text") == "done")

    reg.register(
        "noneResult",
        "Return no payload",
        {"type": "object", "properties": {}},
        lambda: None,
    )
    none_result = json.loads(reg.execute("noneResult", "{}"))
    _check("tool registry rejects empty tool results explicitly",
           "returned no result" in none_result.get("error", ""))

    class _NoResultEditorApi:
        def __init__(self):
            self._window = object()

        def editor_get_content(self):
            return None

    reg_editor_none = ToolRegistry()
    register_engine_tools(reg_editor_none, _FakeGui(), _NoResultEditorApi())
    editor_none = json.loads(reg_editor_none.execute("editor_getContent", "{}"))
    _check("editor tool rejects empty bridge responses explicitly",
           "returned no result" in editor_none.get("error", ""))

    result = reg.execute("nonexistent_tool", "{}")
    data = json.loads(result)
    _check("unknown tool error", "error" in data)


def test_mcp_client() -> None:
    print("── MCP Client ──")
    import io
    from ai_editor.mcp_client import (
        InternalMcpProvider,
        McpManager,
        _parse_sse_event_payloads,
        _read_rpc_message_from_stream,
        _resolve_sse_session_url,
    )

    manager = McpManager()
    manager.register_internal(
        "alpha",
        [{"name": "echo", "description": "Alpha echo", "inputSchema": {}}],
        {"echo": lambda **kw: {"server": "alpha", "args": kw}},
    )
    manager.register_internal(
        "alpha_beta",
        [{"name": "echo", "description": "Alpha beta echo", "inputSchema": {}}],
        {"echo": lambda **kw: {"server": "alpha_beta", "args": kw}},
    )

    tool_map = {
        item["function"]["name"]: item["function"]["parameters"]
        for item in manager.to_openai_tools()
    }
    _check("MCP OpenAI schemas normalize empty inputSchema",
           tool_map.get("mcp_alpha_echo", {}).get("type") == "object"
           and isinstance(tool_map.get("mcp_alpha_echo", {}).get("properties"), dict))

    resolved = json.loads(manager.call_tool("mcp_alpha_beta_echo", {"value": "pong"}))
    _check("MCP tool resolution prefers exact longest server id",
           resolved.get("server") == "alpha_beta"
           and resolved.get("args", {}).get("value") == "pong")

    bad_args = json.loads(manager.call_tool("mcp_alpha_echo", []))
    _check("MCP manager rejects non-object arguments explicitly",
           "must be a JSON object" in bad_args.get("error", ""))

    missing = json.loads(manager.call_tool("mcp_alpha_missing", {}))
    _check("MCP missing tool error is explicit",
           "Available:" in missing.get("error", "")
           and "echo" in missing.get("error", ""))

    manager.register_internal(
        "single_arg",
        [{"name": "echo", "description": "Single payload", "inputSchema": {}}],
        {"echo": lambda payload: {"value": payload.get("value")}},
    )
    single_arg = json.loads(manager.call_tool("mcp_single_arg_echo", {"value": "pong"}))
    _check("internal MCP handler dispatch supports single payload arg",
           single_arg.get("value") == "pong")

    stopped_provider = InternalMcpProvider("stopped")
    stopped_provider.add_tool("echo", "Echo", {}, lambda **kw: kw)
    stopped_provider.stop()
    stopped_call = json.loads(stopped_provider.call_tool("echo", {}))
    _check("internal MCP stop is explicit and disables calls",
           stopped_provider.is_alive is False
           and "not connected" in stopped_call.get("error", ""))

    empty_provider = InternalMcpProvider("empty")
    empty_provider.add_tool("echo", "Echo", {}, lambda **kw: None)
    empty_result = json.loads(empty_provider.call_tool("echo", {}))
    _check("internal MCP empty handler result is explicit error",
           "returned no result" in empty_result.get("error", ""))

    class _FailingHttpClient:
        def post(self, *args, **kwargs):
            raise RuntimeError("boom")

    from ai_editor.mcp_client import McpServerConfig, McpSseClient
    sse_client = McpSseClient(McpServerConfig(
        id="sse_fail",
        name="SSE Fail",
        transport="sse",
        url="http://localhost:3000/mcp",
    ))
    sse_client._alive = True
    sse_client._http = _FailingHttpClient()
    sse_error = json.loads(sse_client.call_tool("echo", {}))
    _check("MCP SSE transport errors are explicit",
           "MCP call failed" in sse_error.get("error", "")
           and sse_error.get("rpc_error", {}).get("data", {}).get("serverId") == "sse_fail")

    framed_payload = b'{"jsonrpc":"2.0","id":1,"result":{"ok":true}}'
    framed_stream = io.BytesIO(
        b"Content-Length: " + str(len(framed_payload)).encode("ascii")
        + b"\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n"
        + framed_payload
    )
    parsed_message = _read_rpc_message_from_stream(framed_stream)
    parsed_json = json.loads(parsed_message.decode("utf-8")) if parsed_message else {}
    _check("MCP stdio parser handles Content-Length framing",
           parsed_json.get("result", {}).get("ok") is True)

    sse_text = "event: endpoint\ndata: {\"sessionUrl\":\"session/abc\"}\n\n"
    sse_payloads = _parse_sse_event_payloads(sse_text)
    session_url = _resolve_sse_session_url("http://localhost:3000/mcp", sse_text)
    _check("MCP SSE parser extracts session URL",
           sse_payloads == ['{"sessionUrl":"session/abc"}']
           and session_url == "http://localhost:3000/session/abc")


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

    compat = LLMEngine(ProviderConfig(provider="openai", base_url="api.openai.com/v1"))
    _check("scheme-less base_url normalized",
           compat._resolve_request_url(compat.config, "/chat/completions")
           == "https://api.openai.com/v1/chat/completions")

    relative = LLMEngine(ProviderConfig(provider="openai", base_url="/v1"))
    _check("relative base_url anchored to provider default host",
           relative._resolve_request_url(relative.config, "/chat/completions")
           == "https://api.openai.com/v1/chat/completions")

    direct = LLMEngine(ProviderConfig(
        provider="custom",
        base_url="https://proxy.example/v1/chat/completions",
    ))
    _check("direct endpoint base_url preserved",
           direct._resolve_request_url(direct.config, "/chat/completions")
           == "https://proxy.example/v1/chat/completions")

    try:
        LLMEngine(ProviderConfig(provider="custom", base_url=""))._resolve_request_url(
            ProviderConfig(provider="custom", base_url=""), "/chat/completions")
        invalid_url_ok = False
    except ValueError as exc:
        invalid_url_ok = "No base URL configured" in str(exc)
    _check("missing custom base_url rejected explicitly", invalid_url_ok)

    chat_payload = {
        "choices": [{"message": {"tool_calls": [{
            "id": "call_1",
            "function": {"name": "readFile", "arguments": {"path": "a.py"}},
        }]}}],
        "usage": {"prompt_tokens": 11, "completion_tokens": 3},
    }
    parsed = LLMEngine._parse_response(chat_payload, e.config)
    _check("ChatCompletions tool args coerced to JSON string",
           parsed.tool_calls[0].arguments == '{"path": "a.py"}')
    _check("ChatCompletions usage normalized",
           parsed.usage.get("input_tokens") == 11
           and parsed.usage.get("output_tokens") == 3
           and parsed.usage.get("total_tokens") == 14)

    responses_payload = {
        "model": "gpt-test",
        "status": "completed",
        "output": [
            {"type": "message", "content": [{"type": "output_text", "text": "ok"}]},
            {"type": "function_call", "call_id": "call_2", "name": "searchFiles",
             "arguments": {"query": "needle"}},
        ],
        "usage": {"input_tokens": 9, "output_tokens": 4},
    }
    parsed_responses = LLMEngine._parse_responses_response(responses_payload, e.config)
    _check("Responses function_call parsed",
           parsed_responses.content == "ok"
           and parsed_responses.tool_calls[0].name == "searchFiles"
           and parsed_responses.tool_calls[0].arguments == '{"query": "needle"}')
    _check("Responses usage total filled",
           parsed_responses.usage.get("total_tokens") == 13)

    token_msgs = [{"role": "assistant", "content": "", "tool_calls": [{
        "id": "call_3",
        "type": "function",
        "function": {
            "name": "engine",
            "arguments": json.dumps({"action": "eval", "expression": "x" * 400}),
        },
    }]}]
    _check("token counter includes tool call arguments",
           e.count_message_tokens(token_msgs) > e.count_message_tokens([{"role": "assistant", "content": ""}]))

    anthropic_tool = LLMEngine._convert_msg_to_anthropic({
        "role": "assistant",
        "content": "",
        "tool_calls": [{"id": "call_bad", "function": {"name": "x", "arguments": "{"}}],
    })
    _check("invalid tool args are not silently emptied",
           "_raw_arguments" in anthropic_tool["content"][0]["input"])


def test_conversation() -> None:
    print("── Conversation ──")
    from ai_editor.chat_state import Conversation, ChatMessage
    from ai_editor.llm_engine import ToolCall

    conv = Conversation(system_prompt="Test system")
    conv.add_message(ChatMessage(role="user", content="Hello"))
    conv.add_message(ChatMessage(role="assistant", content="Hi", tool_calls=[
        ToolCall(id="tc1", name="get_time", arguments='{"tz":"UTC"}'),
    ]))
    conv.add_message(ChatMessage(role="tool", content='{"time":"12:00"}', tool_call_id="tc1",
                                 tool_name="get_time"))

    msgs = conv.to_api_messages()
    _check(f"API messages count: {len(msgs)}", len(msgs) == 4)  # system + 3
    _check("System message", msgs[0]["role"] == "system")
    _check("Tool calls in assistant", "tool_calls" in msgs[2])
    _check("Tool result", msgs[3]["role"] == "tool")
    _check("Tool result keeps tool name", msgs[3].get("name") == "get_time")
    _check("Title auto-set", conv.title == "Hello")

    from ai_editor.chat_state import _compress_tool_result
    big_json = json.dumps({"items": [{"name": "item", "body": "x" * 200} for _ in range(80)]})
    compressed = _compress_tool_result(big_json, "searchFiles")
    compressed_data = json.loads(compressed)
    _check("large JSON tool result compressed",
           compressed_data.get("_compressed") is True
           and len(compressed) < len(big_json))


def test_chat_controller_tool_loop() -> None:
    print("── Chat Controller Tool Loop ──")
    from ai_editor.chat_state import ChatController, Conversation, ChatMessage
    from ai_editor.llm_engine import LLMResponse, ProviderConfig, StreamDelta, ToolCall
    from ai_editor.tool_registry import ToolRegistry

    class _ScriptedEngine:
        def __init__(self) -> None:
            self.config = ProviderConfig(provider="openai", model="tool-loop-test")
            self.requests = []
            self.responses = [
                LLMResponse(tool_calls=[ToolCall(
                    id="call_lookup",
                    name="lookup",
                    arguments='{"topic":"boss"}',
                )]),
                LLMResponse(content="Resolved after tool."),
            ]

        def reset_cancel(self) -> None:
            return None

        def cancel(self) -> None:
            return None

        def count_message_tokens(self, _messages) -> int:
            return 0

        def estimate_tokens(self, text: str) -> int:
            return max(1, len(text) // 4) if text else 0

        def chat_completion_stream(self, messages, tools=None, on_delta=None):
            self.requests.append({"messages": messages, "tools": tools})
            resp = self.responses.pop(0)
            if on_delta and resp.content:
                on_delta(StreamDelta(content=resp.content))
            return resp

    reg = ToolRegistry()
    reg.register(
        "lookup",
        "Lookup test data",
        {
            "type": "object",
            "properties": {"topic": {"type": "string"}},
            "required": ["topic"],
        },
        lambda topic: {"topic": topic, "result": 42},
        category="tests",
    )

    engine = _ScriptedEngine()
    conv = Conversation(system_prompt="loop")
    conv.add_message(ChatMessage(role="user", content="Need tool help"))
    ctrl = ChatController(engine, reg, conv)
    stream_ends = []
    tool_ends = []
    ctrl.on_stream_end = lambda msg: stream_ends.append({
        "content": msg.content,
        "tool_calls": [tc.name for tc in msg.tool_calls],
    })
    ctrl.on_tool_end = lambda cid, name, result, state: tool_ends.append({
        "id": cid,
        "name": name,
        "result": result,
        "state": state,
    })
    ctrl._running = True
    ctrl._run_loop()

    second_request = engine.requests[1]["messages"] if len(engine.requests) > 1 else []
    tool_messages = [m for m in second_request if m.get("role") == "tool"]
    _check("tool loop made a second LLM round",
           len(engine.requests) == 2 and len(stream_ends) == 2)
    _check("second round includes tool result message",
           len(tool_messages) == 1
           and tool_messages[0].get("tool_call_id") == "call_lookup"
           and tool_messages[0].get("name") == "lookup")
    _check("tool end callback includes id name result and state",
           len(tool_ends) == 1
           and tool_ends[0].get("id") == "call_lookup"
           and tool_ends[0].get("name") == "lookup"
           and "result" in json.loads(tool_ends[0].get("result", "{}"))
           and tool_ends[0].get("state") == "completed")
    _check("tool loop resumes with final assistant content",
           stream_ends[-1]["content"] == "Resolved after tool.")


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

    try:
        load_conversation("../escape")
        _check("history blocks path traversal", False)
    except ValueError:
        _check("history blocks path traversal", True)


def test_bridge() -> None:
    print("── WebView Bridge ──")
    from ai_editor.webview_bridge import AIEditorBridge

    events = []
    bridge = AIEditorBridge(_SettingsGui({"ai_editor": {"mode": "ask", "permissions": {"readFile": "allowed"}}}),
                            lambda n, p: events.append((n, p)))

    result = bridge.handle_command("ai_editor_load_config", {})
    _check("load_config", "provider" in result)

    result = bridge.handle_command("ai_editor_list_tools", {})
    _check(f"list_tools: {len(result.get('tools', []))}", len(result.get("tools", [])) >= 10)

    mode_preview = bridge.handle_command("ai_editor_get_mode", {"mode": "plan"})
    _check("bridge get_mode returns computed permissions",
           mode_preview.get("mode") == "ask"
           and mode_preview.get("selected_mode") == "plan"
           and mode_preview.get("defaults", {}).get("runTerminal") == "confirm")

    set_mode = bridge.handle_command("ai_editor_set_mode", {"mode": "agent"})
    _check("bridge set_mode persists",
           set_mode.get("ok") is True
           and bridge.handle_command("ai_editor_get_mode", {}).get("mode") == "agent")

    failing_bridge = AIEditorBridge(
        _FailingSettingsGui({"ai_editor": {}}),
        lambda n, p: None,
    )
    bridge_save = failing_bridge.handle_command(
        "ai_editor_save_config", {"provider": "openai"}
    )
    _check("bridge save_config surfaces persistence errors",
           bridge_save.get("error") == "disk failed"
           and bridge_save.get("applied") is True)

    bridge._ensure_engine()
    sent = []
    bridge._controller.send = lambda text, agent_mode=False: sent.append(
        (text, agent_mode, bridge._controller.conversation.system_prompt))
    bridge.handle_command("ai_editor_set_mode", {"mode": "plan"})
    bridge.handle_command("ai_editor_send", {"text": "hello bridge"})
    _check("bridge send applies current mode prompt",
           sent and sent[-1][0] == "hello bridge"
           and sent[-1][1] is False
           and "<!-- plan_ready -->" in sent[-1][2])

    bridge._confirmation_timeout = 0.0
    denied = bridge._on_tool_confirm("bridge-call", "runTerminal", "{}")
    stale = bridge.handle_command("ai_editor_confirm_tool", {"id": "bridge-call", "allowed": True})
    _check("bridge tool confirm fails closed when UI does not answer",
           denied is False and stale.get("error") == "No pending confirmation")

    bridge._confirmation_timeout = 1.0
    allowed_result = {}

    def _await_bridge_confirm():
        allowed_result["value"] = bridge._on_tool_confirm(
            "bridge-call-2", "runTerminal", "{}")

    confirm_thread = threading.Thread(target=_await_bridge_confirm, daemon=True)
    confirm_thread.start()
    _wait_until(lambda: "bridge-call-2" in bridge._pending_confirm)
    confirm_ok = bridge.handle_command(
        "ai_editor_confirm_tool", {"id": "bridge-call-2", "allowed": True})
    confirm_thread.join(timeout=1.0)
    _check("bridge tool confirm handshake works",
           confirm_ok.get("ok") is True and allowed_result.get("value") is True)

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

        r = save_instruction_file("../escape.md", "nope", workspace_root=tmpdir)
        _check("instruction save blocks path traversal", r.get("ok") is False)

        r = delete_instruction_file("../escape.md", workspace_root=tmpdir)
        _check("instruction delete blocks path traversal", r.get("ok") is False)

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
    uri3 = Uri.parse("untitled:Untitled-1")
    _check("Uri.parse untitled", uri3.scheme == "untitled" and str(uri3) == "untitled:Untitled-1")
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
            "views": {"explorer": [{"id": "test.tree", "name": "Test Tree"}]},
            "menus": {"view/title": [{
                "command": "test.hello",
                "when": "view == test.tree",
                "group": "navigation@1",
            }]},
            "jsonValidation": [{"fileMatch": "test.json", "url": "./schema.json"}],
            "viewsWelcome": [{"view": "test.view", "contents": "Welcome"}],
            "submenus": [{"id": "test.submenu", "label": "Submenu"}],
            "problemMatchers": [{"name": "testMatcher", "pattern": "test"}],
            "breakpoints": [{"language": "python"}],
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
    unsupported_cmd = cmds.execute("test.hello", {"x": 1})
    fallback_kinds = {item.get("kind") for item in unsupported_cmd.get("availableFallbacks", [])}
    _check("EP.command fallback metadata is explicit",
           unsupported_cmd.get("handledBy") == "manifestFallback"
           and unsupported_cmd.get("needsExtensionRuntime") is True
           and unsupported_cmd.get("arguments") == [{"x": 1}]
           and fallback_kinds == {"chatParticipant", "languageModelTool", "view"})
    _check("EP.command runtime support marked",
           ep.command_contributions[0].get("_runtimeSupport", {}).get("needsExtensionRuntime") is True)
    _check("EP.lm_tool runtime support marked",
           ep.language_model_tools[0].get("_runtimeSupport", {}).get("needsExtensionRuntime") is True)
    summary = ep.to_summary()
    _check("EP.summary", summary["chatParticipants"] == 1
            and summary.get("needsExtensionRuntime", {}).get("languageModelTools") == 1)
    _check("EP.summary command runtime count",
           summary.get("commandsContributed") == 1
            and summary.get("needsExtensionRuntime", {}).get("commands") == 1
            and summary.get("manifestCommandFallbacks") == 0)
    _check("EP.summary view runtime count",
           summary.get("needsExtensionRuntime", {}).get("views") == 1)
    contrib_details = ep.all_contributions
    _check("EP.extra contribution details retained",
           len(contrib_details.get("jsonValidation", [])) == 1
            and len(contrib_details.get("views", {}).get("explorer", [])) == 1
           and len(contrib_details.get("menus", {}).get("view/title", [])) == 1
           and len(contrib_details.get("viewsWelcome", [])) == 1
           and len(contrib_details.get("submenus", [])) == 1
           and len(contrib_details.get("problemMatchers", [])) == 1
           and len(contrib_details.get("breakpoints", [])) == 1)

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
    from ai_editor.extension_host import ExtensionHost, ExtensionDescription
    from ai_editor.vscode_api import (
        VscodeNamespace, LanguageModelChat, ChatParticipant,
        ChatRequest, ChatContext, ChatResponseStream, ChatResult,
        WorkspaceConfiguration, LanguageModelToolResult,
        AuthenticationProviderBase, Diagnostic, WorkspaceEdit,
        Position, Range, AuthenticationSession, PreparedToolInvocation,
        EventEmitter,
    )

    host = ExtensionHost()
    ns = VscodeNamespace(host)
    api = ns.build()

    # Namespace structure
    _check("api.commands", "registerCommand" in api["commands"])
    _check("api.window", "showInformationMessage" in api["window"])
    _check("api.workspace", "getConfiguration" in api["workspace"])
    _check("api.env", api["env"]["appName"] == "SAO AI Editor")
    _check("api.languages", "createDiagnosticCollection" in api["languages"])
    _check("api.tasks", "registerTaskProvider" in api["tasks"])
    _check("api.debug", "registerDebugConfigurationProvider" in api["debug"])
    _check("api.lm", "selectChatModels" in api["lm"])
    _check("api.chat", "createChatParticipant" in api["chat"])
    _check("api.authentication", "getSession" in api["authentication"])
    auth_base = AuthenticationProviderBase()
    _check("AuthenticationProviderBase has explicit empty behavior",
           auth_base.get_sessions() == []
           and auth_base.create_session() is None
           and auth_base.remove_session("missing") is False)

    # Types
    _check("api.Position", api["Position"] is not None)
    _check("api.Uri", api["Uri"] is not None)
    _check("api.Disposable", api["Disposable"] is not None)

    # Commands via namespace
    box = []
    command_disposable = api["commands"]["registerCommand"]("test.api", lambda: box.append(1) or "ok")
    r = api["commands"]["executeCommand"]("test.api")
    _check("ns.commands.execute", r == "ok" and box == [1])
    _check("registerCommand returns Disposable", hasattr(command_disposable, "dispose"))

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
    cp.dispose()
    _check("participant dispose removes registry entry", "test-bot" not in ns.chat_participants)

    # LM tool
    class MyTool:
        def invoke(self, options, token):
            return LanguageModelToolResult.text(f"result:{options.input}")
    dispose = api["lm"]["registerTool"]("my_tool", MyTool())
    _check("registerTool", "my_tool" in ns.registered_tools)
    tool_result = api["lm"]["invokeTool"]("my_tool", {"value": 7})
    _check("invokeTool passes options input",
           tool_result.content[0]["text"] == "result:{'value': 7}")
    dispose.dispose()
    _check("tool disposed", "my_tool" not in ns.registered_tools)

    api["lm"]["registerToolDefinition"]("schema_only", {"type": "object"})
    schema_result = api["lm"]["invokeTool"]("schema_only", {})
    _check("schema-only tool is explicit unsupported",
            "needsExtensionRuntime" in schema_result.content[0]["text"]
            and "registerTool('schema_only'" in schema_result.content[0]["text"])

    api["lm"]["registerToolDefinition"]("merged_tool", {
        "type": "object",
        "properties": {"value": {"type": "string"}},
        "required": ["value"],
    })

    class MergeTool:
        def invoke(self, options, token):
            return LanguageModelToolResult.text(options.input.get("value", ""))

    api["lm"]["registerTool"]("merged_tool", MergeTool())
    merged_meta = {item["name"]: item for item in api["lm"]["getTools"]()}
    _check("tool definition merged into runtime tool",
           merged_meta["merged_tool"].get("inputSchema", {}).get("required") == ["value"]
           and not merged_meta["merged_tool"].get("needsExtensionRuntime"))
    merged_result = api["lm"]["invokeTool"]("merged_tool", {"value": "merged"})
    _check("merged runtime tool callable",
           merged_result.content[0]["text"] == "merged")

    class ConfirmTool:
        def __init__(self) -> None:
            self.invoked = 0

        def prepareInvocation(self, options, token):
            return PreparedToolInvocation({
                "title": "Confirm tool",
                "message": "Run confirm_tool?",
            })

        def invoke(self, options, token):
            self.invoked += 1
            return LanguageModelToolResult.text("confirmed")

    confirm_tool = ConfirmTool()
    api["lm"]["registerTool"]("confirm_tool", confirm_tool)
    denied_result = api["lm"]["invokeTool"]("confirm_tool", {})
    _check("prepareInvocation requires UI confirmation",
           confirm_tool.invoked == 0
           and "requiresConfirmation" in denied_result.content[0]["text"])

    class ConfirmBridge:
        def confirm_tool_invocation(self, tool_name, confirmation, input_data):
            return tool_name == "confirm_tool" and confirmation.get("title") == "Confirm tool"

    ns.set_ui_bridge(ConfirmBridge())
    confirmed_result = api["lm"]["invokeTool"]("confirm_tool", {})
    _check("prepareInvocation uses UI confirmation bridge",
           confirm_tool.invoked == 1
           and confirmed_result.content[0]["text"] == "confirmed")
    ns.set_ui_bridge(None)

    # Variable
    api["chat"]["registerVariable"]("testvar", "A test variable",
                                     lambda: "var_value")
    _check("registerVariable", "testvar" in ns.variables)
    chat_ctx = api["chat"]["registerChatWorkspaceContextProvider"](
        "ctx.workspace", {"label": "Workspace Context"})
    _check("chat context providers are retained",
           "ctx.workspace" in api["chat"]["contextProviders"]["workspace"])
    chat_ctx.dispose()
    _check("chat context provider disposal works",
           "ctx.workspace" not in api["chat"]["contextProviders"]["workspace"])

    with tempfile.TemporaryDirectory() as tmpdir:
        ns._get_root_path = lambda: tmpdir
        sample = os.path.join(tmpdir, "sample.py")
        with open(sample, "w", encoding="utf-8") as fh:
            fh.write("print('vscode')\n")
        doc = api["workspace"]["openTextDocument"](sample)
        found = api["workspace"]["findFiles"]("*.py")
        found_recursive = api["workspace"]["findFiles"]("**/*.py")
        opened_docs = []
        api["workspace"]["onDidOpenTextDocument"](
            lambda opened: opened_docs.append(opened.fileName))
        untitled = api["workspace"]["openTextDocument"](
            {"content": "hello", "language": "markdown"})
        _check("workspace.openTextDocument reads real files",
               doc.getText() == "print('vscode')\n"
               and doc.languageId == "python")
        _check("workspace tracks open text documents",
               sample in [item.fileName for item in api["workspace"]["textDocuments"]]
               and untitled.isUntitled is True
               and untitled.languageId == "markdown"
               and untitled.fileName == "untitled:Untitled-1")
        _check("workspace open event fires for new documents",
               "untitled:Untitled-1" in opened_docs)
        _check("workspace.findFiles returns real matches",
               len(found) == 1 and found[0].fs_path == sample
               and len(found_recursive) == 1)
        edit = WorkspaceEdit()
        edit.replace(doc.uri, Range(Position(0, 0), Position(0, 5)), "echo")
        _check("workspace.applyEdit rejects malformed payloads",
             api["workspace"]["applyEdit"]({"edits": [1]}) is False)
        _check("workspace.applyEdit updates document and file contents",
             api["workspace"]["applyEdit"](edit) is True
             and doc.getText().startswith("echo('vscode')")
             and "echo('vscode')" in open(sample, "r", encoding="utf-8").read())
        _check("workspace.applyEdit empty WorkspaceEdit succeeds",
               api["workspace"]["applyEdit"](WorkspaceEdit()) is True)
        class _MemFS:
            def __init__(self):
                self.files = {"/note.txt": b"hello"}

            def readFile(self, uri):
                return self.files.get(uri.path, b"")

            def writeFile(self, uri, content):
                self.files[uri.path] = bytes(content)

            def stat(self, uri):
                content = self.files.get(uri.path, b"")
                return {"type": 1, "size": len(content), "mtime": 0}

            def readDirectory(self, uri):
                prefix = uri.path.rstrip("/") + "/"
                result = []
                for path in sorted(self.files):
                    if not path.startswith(prefix):
                        continue
                    name = path[len(prefix):]
                    if "/" not in name:
                        result.append((name, 1))
                return result

            def createDirectory(self, uri):
                return None

            def delete(self, uri, options=None):
                self.files.pop(uri.path, None)

            def rename(self, old_uri, new_uri, options=None):
                self.files[new_uri.path] = self.files.pop(old_uri.path, b"")

        memfs = _MemFS()
        api["workspace"]["registerFileSystemProvider"]("memfs", memfs)
        mem_uri = api["Uri"].parse("memfs:/note.txt")
        mem_doc = api["workspace"]["openTextDocument"](mem_uri)
        mem_edit = WorkspaceEdit()
        mem_edit.replace(mem_uri, Range(Position(0, 0), Position(0, 5)), "HELLO")
        _check("workspace.registerFileSystemProvider supports custom scheme edits",
               api["workspace"]["applyEdit"](mem_edit) is True
               and mem_doc.getText() == "HELLO"
               and api["workspace"]["fs"].readFile(mem_uri) == b"HELLO")
        watcher = api["workspace"]["createFileSystemWatcher"]("*.py", True)
        watcher_events = []
        watcher.onDidCreate(lambda uri: watcher_events.append(uri))
        watcher._create.fire("sample.py")
        _check("workspace.createFileSystemWatcher exposes events",
               watcher_events == ["sample.py"] and watcher.ignoreCreateEvents is True)
        fs_events = []
        fs_watcher = api["workspace"]["createFileSystemWatcher"]("*.txt")
        fs_watcher.onDidCreate(lambda uri: fs_events.append(("create", os.path.basename(uri.fs_path))))
        fs_watcher.onDidChange(lambda uri: fs_events.append(("change", os.path.basename(uri.fs_path))))
        fs_watcher.onDidDelete(lambda uri: fs_events.append(("delete", os.path.basename(uri.fs_path))))
        watched_txt = os.path.join(tmpdir, "watch.txt")
        with open(watched_txt, "w", encoding="utf-8") as fh:
            fh.write("one")
        changed = _wait_until(lambda: ("create", "watch.txt") in fs_events)
        with open(watched_txt, "a", encoding="utf-8") as fh:
            fh.write("two")
        changed = _wait_until(lambda: changed and ("change", "watch.txt") in fs_events)
        os.remove(watched_txt)
        changed = _wait_until(lambda: changed and ("delete", "watch.txt") in fs_events)
        _check("workspace.createFileSystemWatcher tracks real file changes",
               changed)
        _check("workspace.asRelativePath",
               api["workspace"]["asRelativePath"](sample) == "sample.py")
        active_events = []
        visible_events = []
        api["window"]["onDidChangeActiveTextEditor"](
            lambda active: active_events.append(active.document.fileName if active else ""))
        api["window"]["onDidChangeVisibleTextEditors"](
            lambda editors: visible_events.append(len(editors)))
        editor = api["window"]["showTextDocument"](doc)
        _check("window.showTextDocument returns an editor",
               editor.document is doc and editor.viewColumn == 1)
        _check("window active editor state stays live",
               api["window"]["activeTextEditor"] is editor
               and api["window"]["visibleTextEditors"] == [editor]
               and active_events == [sample]
               and visible_events == [1])
        editor_hits = []
        text_editor_disposable = api["commands"]["registerTextEditorCommand"](
            "test.editor", lambda active, edit: editor_hits.append(active.document.fileName))
        api["commands"]["executeCommand"]("test.editor")
        _check("registerTextEditorCommand receives active editor",
               editor_hits == [sample])
        _check("registerTextEditorCommand returns Disposable",
               hasattr(text_editor_disposable, "dispose"))
        api["env"]["clipboard"]["writeText"]("clip-value")
        _check("env.clipboard round-trips text",
               api["env"]["clipboard"]["readText"]() == "clip-value")
        opener_target = (
            "ai_editor.vscode_api.os.startfile"
            if os.name == "nt" else "ai_editor.vscode_api.webbrowser.open"
        )
        with patch(opener_target, return_value=None, create=True) as open_external_mock:
            ext_result = api["env"]["openExternal"]("https://example.invalid")
        _check("env.openExternal returns bool without launching browser",
               isinstance(ext_result, bool) and open_external_mock.call_count == 1)

        diagnostics = api["languages"]["createDiagnosticCollection"]("selftest")
        diagnostics.set("file:///tmp/a.py", [{"message": "bad"}])
        _check("languages.createDiagnosticCollection stores diagnostics",
               diagnostics.get("file:///tmp/a.py")[0].get("message") == "bad")
        diag = Diagnostic(
            Range(Position(0, 0), Position(0, 1)), "boom",
            api["DiagnosticSeverity"]["Error"])
        diagnostics.set(doc.uri, [diag])
        _check("languages.getDiagnostics reads collections",
               api["languages"]["getDiagnostics"](doc.uri)[0].message == "boom")
        provider_dispose = api["languages"]["registerHoverProvider"]("python", object())
        _check("languages provider registration disposable", hasattr(provider_dispose, "dispose"))
        _check("languages.match", api["languages"]["match"]("python", doc) == 10)
        class _TaskProvider:
            def provideTasks(self):
                return [{
                    "type": "selftest",
                    "label": "echo task",
                    "command": sys.executable,
                    "args": ["-c", "print('task ok')"],
                }]

        task_start = []
        task_end = []
        api["tasks"]["onDidStartTask"](lambda evt: task_start.append(evt))
        api["tasks"]["onDidEndTask"](lambda evt: task_end.append(evt))
        api["tasks"]["registerTaskProvider"]("selftest", _TaskProvider())
        fetched_tasks = api["tasks"]["fetchTasks"]({"type": "selftest"})
        task_execution = api["tasks"]["executeTask"](fetched_tasks[0])
        task_done = _wait_until(lambda: task_execution.exitStatus is not None)
        _check("tasks.executeTask runs local commands and updates lifecycle",
               len(fetched_tasks) == 1
               and task_execution.processId is not None
               and task_done
               and task_execution.exitStatus.get("code") == 0
               and len(task_start) == 1
               and len(task_end) == 1
               and api["tasks"]["taskExecutions"] == [])

        class _DebugProvider:
            def resolveDebugConfiguration(self, folder, config):
                return dict(config)

        debug_target = os.path.join(tmpdir, "debug_target.py")
        with open(debug_target, "w", encoding="utf-8") as fh:
            fh.write("print('debug ok')\n")
        debug_start = []
        debug_end = []
        api["debug"]["onDidStartDebugSession"](lambda session: debug_start.append(session))
        api["debug"]["onDidTerminateDebugSession"](lambda session: debug_end.append(session))
        api["debug"]["registerDebugConfigurationProvider"]("python", _DebugProvider())
        debug_started = api["debug"]["startDebugging"](None, {
            "type": "python",
            "name": "selftest-debug",
            "program": debug_target,
        })
        debug_done = _wait_until(lambda: len(debug_end) == 1)
        _check("debug.startDebugging launches local python program",
               debug_started is True
               and len(debug_start) == 1
               and debug_done
               and debug_start[0].type == "python"
               and debug_end[0].exitStatus.get("code") == 0
               and api["debug"]["activeDebugSession"] is None)
        manifest_desc = ExtensionDescription.from_package_json({
            "name": "task-debug-manifest",
            "publisher": "test",
            "version": "0.0.1",
            "contributes": {
                "taskDefinitions": [{"type": "selftest-task", "required": ["command"]}],
                "debuggers": [{"type": "selftest-debug", "label": "Selftest Debugger"}],
            },
        }, tmpdir)
        host.ext_points.process(manifest_desc)
        api["tasks"] = ns._build_tasks()
        api["debug"] = ns._build_debug()
        _check("manifest task/debug contributions are surfaced",
               any(item.get("type") == "selftest-task" for item in api["tasks"]["taskDefinitions"])
               and any(item.get("type") == "selftest-debug" for item in api["debug"]["debuggers"]))
        diagnostics.clear()
        _check("languages diagnostics clear", diagnostics.get("file:///tmp/a.py") == [])
        ext_changes = []
        api["extensions"]["onDidChange"](lambda evt: ext_changes.append(evt))
        dyn_ext_dir = os.path.join(tmpdir, "dynamic-ext")
        os.makedirs(dyn_ext_dir, exist_ok=True)
        with open(os.path.join(dyn_ext_dir, "package.json"), "w", encoding="utf-8") as fh:
            json.dump({"name": "dynamic-ext", "publisher": "test", "version": "0.0.1"}, fh)
        host.install_from_dir(dyn_ext_dir)
        _check("extensions API updates on host changes",
               api["extensions"]["getExtension"]("test.dynamic-ext") is not None
               and any(item.id == "test.dynamic-ext" for item in api["extensions"]["all"])
               and len(ext_changes) >= 1)

        panel = api["window"]["createWebviewPanel"]("test", "Test", 1)
        panel.webview.html = "<div>panel-html</div>"
        webview_messages = []
        panel.webview.on_did_receive_message(lambda msg: webview_messages.append(msg))
        posted = panel.webview.post_message({"hello": "webview"})
        _check("webview.post_message stays extension-to-webview only",
            posted is True and webview_messages == [])
        webview_messages_2 = []
        panel.webview.onDidReceiveMessage(lambda msg: webview_messages_2.append(msg))
        posted_2 = panel.webview.postMessage({"hello": "camel"})
        _check("webview camelCase postMessage stays outbound only",
            posted_2 is True and webview_messages_2 == [])
        delivered = ns.deliver_webview_message("test", {"from": "ui"})
        _check("webview panel routes UI messages and HTML through namespace",
            delivered is True
            and webview_messages_2[-1] == {"from": "ui"}
            and ns.get_webview_html("test") == "<div>panel-html</div>")
    tree_changes = []
    ns.set_tree_view_change_callback(lambda evt: tree_changes.append(evt))

    class _RefreshTreeProvider:
        def __init__(self):
            self._emitter = EventEmitter()

        @property
        def onDidChangeTreeData(self):
            return self._emitter.event

        def getChildren(self, element=None):
            return ["node"]

        def refresh(self, element=None):
            self._emitter.fire(element)

    tree_provider = _RefreshTreeProvider()
    tree_view = api["window"]["createTreeView"](
        "selftest.tree", treeDataProvider=tree_provider)
    tree_selection_events = []
    tree_view.onDidChangeSelection(
        lambda evt: tree_selection_events.append(evt))
    api["window"]["registerTreeDataProvider"]("selftest.tree", tree_provider)
    tree_view.reveal("node")
    _check("tree view stores provider and selection",
           tree_view.provider is tree_provider and tree_view.selection == ["node"])
    _check("tree view selection emits VSCode event",
           tree_selection_events
           and tree_selection_events[-1].get("selection") == ["node"])
    _check("tree view reveal marks focused element",
           tree_view.is_revealed("node")
           and tree_view.reveal_version == 1)
    tree_expand_events = []
    tree_collapse_events = []
    tree_view.onDidExpandElement(
        lambda evt: tree_expand_events.append(evt))
    tree_view.onDidCollapseElement(
        lambda evt: tree_collapse_events.append(evt))
    tree_view.begin_snapshot()
    tree_handle = tree_view.remember_element("node")
    tree_expanded = tree_view.set_expanded(tree_handle, True)
    tree_collapsed = tree_view.set_expanded(tree_handle, False)
    _check("tree view expand and collapse events expose element",
           tree_expanded is True
           and tree_collapsed is True
           and tree_expand_events[-1].get("element") == "node"
           and tree_collapse_events[-1].get("element") == "node")
    before_tree_version = tree_view.refresh_version
    tree_provider.refresh("node")
    _check("tree data provider refresh notifies runtime view callback",
           tree_view.refresh_version > before_tree_version
           and any(evt.get("event") == "refresh"
                   and evt.get("view_id") == "selftest.tree"
                   for evt in tree_changes))
    resolved_webviews = []
    class _WebviewViewProvider:
        def resolveWebviewView(self, view, context=None, token=None):
            resolved_webviews.append(view.viewType)
            view.webview.html = "<div>resolved</div>"
    webview_provider_disposable = api["window"]["registerWebviewViewProvider"](
        "selftest.webview", _WebviewViewProvider())
    _check("registerWebviewViewProvider resolves a view",
           resolved_webviews == ["selftest.webview"]
           and hasattr(webview_provider_disposable, "dispose"))

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
    api["lm"]["registerMcpServerDefinitionProvider"]("mcp.selftest", object())
    _check("lm MCP providers stay separate from invocable tools",
           "mcp.selftest" in api["lm"]["mcpServerDefinitionProviders"]
           and all(not item.get("name", "").startswith("mcp:") for item in api["lm"]["getTools"]()))

    # chat context providers
    _check("chat.registerChatWorkspaceContextProvider",
           "registerChatWorkspaceContextProvider" in api["chat"])

    # window.withProgress
    _check("window.withProgress", callable(api["window"]["withProgress"]))
    _check("window.createWebviewPanel", callable(api["window"]["createWebviewPanel"]))

    import ai_editor.auth as auth_module
    from ai_editor.auth import AuthService
    old_auth_singleton = auth_module._singleton
    tmp_auth = tempfile.mkdtemp(prefix="sao_auth_vscode_test_")
    try:
        auth_module._singleton = AuthService(storage_dir=tmp_auth)
        auth_events = []
        api["authentication"]["onDidChangeSessions"](
            lambda evt: auth_events.append(evt))
        auth_module._singleton.create_session_from_token(
            "github", "gh-test", "GitHub", scopes=["read:user"])
        class _AuthProvider(AuthenticationProviderBase):
            def create_session(self, scopes=None, options=None):
                return auth_module._singleton.create_session_from_token(
                    "github-test", "gh-create", "GitHub Test", scopes=scopes or [])
        api["authentication"]["registerAuthenticationProvider"](
            "github-test", "GitHub Test", _AuthProvider())
        class _SessionProvider(AuthenticationProviderBase):
            def get_sessions(self, scopes=None, options=None):
                return [AuthenticationSession(
                    id="cached-session",
                    access_token="cached-token",
                    account={"id": "cached", "label": "Cached"},
                    scopes=scopes or [],
                )]
        api["authentication"]["registerAuthenticationProvider"](
            "cached-test", "Cached Test", _SessionProvider())
        created_auth_session = api["authentication"]["getSession"](
            "github-test", ["repo"], {"createIfNone": True})
        auth_session = api["authentication"]["getSession"](
            "github", ["read:user"], {})
        cached_auth_session = api["authentication"]["getSession"](
            "cached-test", ["read:user"], {})
        _check("authentication.getSession reads AuthService sessions",
               auth_session is not None and auth_session.accessToken == "gh-test")
        _check("authentication.getSession reads registered provider sessions",
               cached_auth_session is not None
               and cached_auth_session.accessToken == "cached-token")
        _check("authentication provider createIfNone emits session event",
               created_auth_session is not None
               and created_auth_session.accessToken == "gh-create"
               and auth_events[-1].get("provider") == "github-test")
    finally:
        auth_module._singleton = old_auth_singleton
        shutil.rmtree(tmp_auth, ignore_errors=True)


def test_app_extension_runtime_support() -> None:
    print("── App Extension Runtime Support ──")
    import ai_editor.extension_host as extension_host_module
    from ai_editor.app import AIEditorAPI
    from ai_editor.chat_providers import ChatProviderDef
    from ai_editor.extension_host import (
        EventEmitter, ExtensionDescription, ExtensionHost)
    from ai_editor.vscode_api import LanguageModelToolResult

    previous_host = extension_host_module._host
    language_tmp = ""
    settings_tmp = ""
    extension_host_module._host = ExtensionHost()
    try:
        api = AIEditorAPI(_SettingsGui({"ai_editor": {}}))
        api._ensure_engine()

        manifest_desc = ExtensionDescription.from_package_json({
            "name": "manifest-only",
            "publisher": "selftest",
            "version": "0.0.1",
            "contributes": {
                "chatParticipants": [{
                    "id": "selftest.manifest.participant",
                    "name": "ManifestOnly",
                    "fullName": "Manifest Only Participant",
                }],
                "languageModelTools": [{
                    "name": "manifest_tool",
                    "displayName": "Manifest Tool",
                    "inputSchema": {"type": "object", "properties": {}},
                }],
            },
        }, "/tmp/selftest-manifest")
        api._ext_host.ext_points.process(manifest_desc)
        api._register_ext_tools()
        ext_contribs = api.get_extension_contributions()
        _check("app exposes extension contribution details",
               ext_contribs.get("summary", {}).get("languageModelTools", 0) >= 1
               and "languageModelTools" in ext_contribs.get("contributions", {}))

        language_tmp = tempfile.mkdtemp(prefix="sao_ext_language_")
        os.makedirs(os.path.join(language_tmp, "themes"), exist_ok=True)
        with open(os.path.join(language_tmp, "themes", "self-dark.json"),
                  "w", encoding="utf-8") as f:
            json.dump({
                "name": "Self Dark",
                "type": "dark",
                "colors": {
                    "editor.background": "#101820",
                    "editor.foreground": "#f0f3f8",
                    "statusBar.background": "#203040",
                },
            }, f)
        language_desc = ExtensionDescription.from_package_json({
            "name": "language-pack",
            "publisher": "selftest",
            "version": "0.0.1",
            "contributes": {
                "languages": [{
                    "id": "selflang",
                    "aliases": ["Self Lang"],
                    "extensions": [".self"],
                    "filenames": ["SELFFILE"],
                }],
                "grammars": [{
                    "language": "selflang",
                    "scopeName": "source.selflang",
                    "path": "./syntaxes/self.tmLanguage.json",
                    "embeddedLanguages": {
                        "meta.embedded.selflang": "json",
                    },
                }],
                "themes": [{
                    "label": "Self Dark",
                    "uiTheme": "vs-dark",
                    "path": "./themes/self-dark.json",
                }],
                "iconThemes": [{
                    "id": "self-icons",
                    "label": "Self Icons",
                    "path": "./themes/self-icons.json",
                }],
            },
        }, language_tmp)
        api._ext_host.registry.register(language_desc)
        api._ext_host.ext_points.process(language_desc)
        editor_languages = {
            item.get("id"): item
            for item in api.list_editor_languages().get("languages", [])
        }
        editor_grammars = api.list_editor_grammars().get("grammars", [])
        editor_themes = api.list_editor_themes()
        editor_theme_data = api.get_editor_theme("Self Dark")
        selflang_grammars = (
            editor_languages.get("selflang", {}).get("grammars") or [{}])
        selflang_resolved_path = (
            selflang_grammars[0]
            .get("resolvedPath", "")
            .replace("\\", "/"))
        _check("extension languages feed editor language table",
               editor_languages.get("selflang", {}).get("name") == "Self Lang"
               and ".self" in editor_languages.get("selflang", {}).get("extensions", [])
               and "source.selflang" in editor_languages.get("selflang", {}).get("grammarScopes", [])
               and editor_languages.get("selflang", {}).get("tokenizer") == "textmate")
        _check("extension grammars feed editor language metadata",
               any(item.get("language") == "selflang"
                   and item.get("scopeName") == "source.selflang"
                   and item.get("embeddedLanguages", {}).get("meta.embedded.selflang") == "json"
                   for item in editor_grammars)
               and selflang_resolved_path.endswith("/syntaxes/self.tmLanguage.json"))
        _check("extension themes feed editor theme metadata",
               editor_themes.get("colorThemes") >= 1
               and editor_themes.get("iconThemes") >= 1
               and any(item.get("label") == "Self Dark"
                       and item.get("uiTheme") == "vs-dark"
                       and item.get("themeType") == "color"
                       for item in editor_themes.get("themes", []))
               and any(item.get("id") == "self-icons"
                       and item.get("themeType") == "icon"
                       for item in editor_themes.get("themes", [])))
        _check("extension color theme JSON feeds editor theme colors",
               editor_theme_data.get("ok") is True
               and editor_theme_data.get("colors", {}).get("editor.background") == "#101820"
               and editor_theme_data.get("theme", {}).get("label") == "Self Dark")
        _check("extension language extensions and filenames drive file detection",
               api._editor_language_for_path("demo.self") == "selflang"
               and api._editor_language_for_path("SELFFILE") == "selflang")

        settings_tmp = tempfile.mkdtemp(prefix="sao_ext_settings_")
        settings_desc = ExtensionDescription.from_package_json({
            "name": "settings-pack",
            "publisher": "selftest",
            "version": "0.0.1",
            "activationEvents": ["*"],
            "contributes": {
                "configuration": {
                    "title": "Selftest Settings",
                    "properties": {
                        "selftest.flag": {
                            "type": "boolean",
                            "default": False,
                            "description": "Feature flag",
                        },
                        "selftest.mode": {
                            "type": "string",
                            "default": "auto",
                            "enum": ["auto", "manual"],
                            "order": 2,
                        },
                        "selftest.options": {
                            "type": "object",
                            "default": {"level": 1},
                        },
                    },
                },
                "configurationDefaults": {
                    "selftest.mode": "manual",
                    "[selflang]": {
                        "editor.tabSize": 2,
                    },
                },
            },
        }, settings_tmp)
        api._ext_host.registry.register(settings_desc)
        api._ext_host.activator.activate(settings_desc.id)
        ext_settings = api.list_extension_settings().get("configurations", [])
        settings_cfg = next(
            (item for item in ext_settings
             if item.get("extension_id") == "selftest.settings-pack"),
            {})
        _check("extension settings expose defaults and modified map",
               settings_cfg.get("values", {}).get("selftest.flag") is False
               and settings_cfg.get("values", {}).get("selftest.mode") == "manual"
               and settings_cfg.get("defaults", {}).get("selftest.mode") == "manual"
               and settings_cfg.get("modified", {}).get("selftest.flag") is False)
        _check("extension configurationDefaults override schema defaults",
               api.get_extension_setting("selftest.mode").get("value") == "manual")
        set_setting = api.set_extension_setting("selftest.flag", True)
        after_set = next(
            (item for item in api.list_extension_settings().get("configurations", [])
             if item.get("extension_id") == "selftest.settings-pack"),
            {})
        _check("extension setting write marks modified override",
               set_setting.get("ok") is True
               and after_set.get("values", {}).get("selftest.flag") is True
               and after_set.get("configuredValues", {}).get("selftest.flag") is True
               and after_set.get("modified", {}).get("selftest.flag") is True)
        reset_setting = api.reset_extension_setting("selftest.flag")
        after_reset = next(
            (item for item in api.list_extension_settings().get("configurations", [])
             if item.get("extension_id") == "selftest.settings-pack"),
            {})
        _check("extension setting reset restores default",
               reset_setting.get("ok") is True
               and after_reset.get("values", {}).get("selftest.flag") is False
               and "selftest.flag" not in after_reset.get("configuredValues", {})
               and after_reset.get("modified", {}).get("selftest.flag") is False)

        manifest_tool_name = api._extension_tool_wrapper_name(
            "selftest.manifest-only", "manifest_tool")
        manifest_tools = {t.get("name"): t for t in api.list_tools().get("tools", [])}
        _check("manifest language model tool exposed in list_tools",
               manifest_tools.get(manifest_tool_name, {}).get("needsExtensionRuntime") is True
               and manifest_tools.get(manifest_tool_name, {}).get("sourceName") == "manifest_tool")
        manifest_exec = json.loads(api.execute_tool(
            manifest_tool_name, "{}", confirmed=True))
        _check("manifest-only language model tool invocation explains runtime need",
               manifest_exec.get("needsExtensionRuntime") is True
               and manifest_exec.get("code") == "needsExtensionRuntime")

        manifest_resp = api.invoke_chat_participant(
            "selftest.manifest.participant", "hello")
        _check("manifest-only participant explicit runtime need",
               manifest_resp.get("needsExtensionRuntime") is True
               and manifest_resp.get("participantId") == "selftest.manifest.participant")
        provider_ids = {p.get("id") for p in api.list_chat_providers().get("providers", [])}
        _check("manifest-only participant not exposed as fake provider",
               "ext-selftest.manifest.participant" not in provider_ids)

        class RuntimeTool:
            description = "Runtime echo tool"
            inputSchema = {
                "type": "object",
                "properties": {"value": {"type": "string"}},
                "required": ["value"],
            }

            def invoke(self, options, token):
                return LanguageModelToolResult.text(
                    "runtime:" + options.input.get("value", ""))

        vscode_api = api._vscode_ns.build(manifest_desc)
        vscode_api["lm"]["registerTool"]("manifest_tool", RuntimeTool())
        vscode_api["lm"]["registerTool"]("runtime_only_tool", RuntimeTool())
        runtime_tools = {t.get("name"): t for t in api.list_tools().get("tools", [])}
        runtime_manifest_tool = runtime_tools.get(manifest_tool_name, {})
        runtime_only_name = api._extension_tool_wrapper_name(
            "selftest.manifest-only", "runtime_only_tool")
        _check("runtime-backed manifest tool reports invocable",
               runtime_manifest_tool.get("runtimeAvailable") is True
               and not runtime_manifest_tool.get("needsExtensionRuntime"))
        runtime_manifest_exec = json.loads(api.execute_tool(
            manifest_tool_name, json.dumps({"value": "pong"}), confirmed=True))
        _check("runtime-backed manifest tool invokes registered handler",
               runtime_manifest_exec.get("ok") is True
               and runtime_manifest_exec.get("content", [{}])[0].get("text") == "runtime:pong")
        runtime_only_exec = json.loads(api.execute_tool(
            runtime_only_name, json.dumps({"value": "direct"}), confirmed=True))
        _check("runtime-only language model tool exposed and invocable",
               runtime_tools.get(runtime_only_name, {}).get("runtimeAvailable") is True
               and runtime_only_exec.get("content", [{}])[0].get("text") == "runtime:direct")

        command_tool_desc = ExtensionDescription.from_package_json({
            "name": "command-tool",
            "publisher": "selftest",
            "version": "0.0.1",
            "activationEvents": ["onCommand:selftest.command.tool"],
            "contributes": {
                "commands": [{"command": "selftest.command.tool", "title": "Tool Command"}],
                "languageModelTools": [{
                    "name": "selftest_command_tool",
                    "displayName": "Command Tool",
                    "inputSchema": {
                        "type": "object",
                        "properties": {"value": {"type": "string"}},
                    },
                }],
            },
        }, "/tmp/selftest-command-tool")
        api._ext_host.ext_points.process(command_tool_desc)
        api._vscode_ns.build(command_tool_desc)["lm"]["registerTool"](
            "selftest_command_tool", RuntimeTool())
        command_tool_result = api._ext_host.commands.execute(
            "selftest.command.tool", {"value": "from-command"})
        _check("command fallback invokes runtime language model tool",
               command_tool_result.get("handledBy") == "runtimeFallback"
               and command_tool_result.get("fallbackKind") == "languageModelTool"
               and command_tool_result.get("content", [{}])[0].get("text") == "runtime:from-command")

        command_chat_desc = ExtensionDescription.from_package_json({
            "name": "command-chat",
            "publisher": "selftest",
            "version": "0.0.1",
            "activationEvents": ["onCommand:selftest.command.chat"],
            "contributes": {
                "commands": [{"command": "selftest.command.chat", "title": "Chat Command"}],
                "chatParticipants": [{
                    "id": "selftest.command.chat.participant",
                    "name": "Command Chat",
                    "fullName": "Command Chat Participant",
                }],
            },
        }, "/tmp/selftest-command-chat")
        api._ext_host.ext_points.process(command_chat_desc)
        def _command_chat_handler(req, ctx, stream, token):
            stream.markdown("chat:" + req.prompt)
            return None
        api._vscode_ns.build(command_chat_desc)["chat"]["createChatParticipant"](
            "selftest.command.chat.participant", _command_chat_handler)
        command_chat_result = api._ext_host.commands.execute(
            "selftest.command.chat", {"prompt": "hello"})
        _check("command fallback invokes runtime chat participant",
               command_chat_result.get("handledBy") == "runtimeFallback"
               and command_chat_result.get("fallbackKind") == "chatParticipant"
               and command_chat_result.get("content") == "chat:hello")

        command_tree_desc = ExtensionDescription.from_package_json({
            "name": "command-tree",
            "publisher": "selftest",
            "version": "0.0.1",
            "activationEvents": ["onCommand:selftest.command.tree"],
            "contributes": {
                "commands": [{"command": "selftest.command.tree", "title": "Tree Command"}],
                "views": {"explorer": [{
                    "id": "selftest.command.tree.view",
                    "name": "Command Tree View",
                }]},
            },
        }, "/tmp/selftest-command-tree")
        api._ext_host.ext_points.process(command_tree_desc)
        class _TreeProvider:
            def getChildren(self, element=None):
                return ["node-a", "node-b"]
        api._vscode_ns.build(command_tree_desc)["window"]["registerTreeDataProvider"](
            "selftest.command.tree.view", _TreeProvider())
        command_tree_result = api._ext_host.commands.execute("selftest.command.tree")
        _check("command fallback resolves runtime tree view",
               command_tree_result.get("handledBy") == "runtimeFallback"
               and command_tree_result.get("fallbackKind") == "treeView"
               and command_tree_result.get("children") == ["node-a", "node-b"])

        command_webview_desc = ExtensionDescription.from_package_json({
            "name": "command-webview",
            "publisher": "selftest",
            "version": "0.0.1",
            "activationEvents": ["onCommand:selftest.command.webview"],
            "contributes": {
                "commands": [{"command": "selftest.command.webview", "title": "Webview Command"}],
                "views": {"panel": [{
                    "id": "selftest.command.webview.view",
                    "name": "Command Webview View",
                    "type": "webview",
                }]},
            },
        }, "/tmp/selftest-command-webview")
        api._ext_host.ext_points.process(command_webview_desc)
        class _CommandWebviewProvider:
            def resolveWebviewView(self, view, context=None, token=None):
                view.webview.html = "<div>command-webview</div>"
        api._vscode_ns.build(command_webview_desc)["window"]["registerWebviewViewProvider"](
            "selftest.command.webview.view", _CommandWebviewProvider())
        command_webview_result = api._ext_host.commands.execute("selftest.command.webview")
        _check("command fallback resolves runtime webview view",
               command_webview_result.get("handledBy") == "runtimeFallback"
               and command_webview_result.get("fallbackKind") == "webviewView"
               and "command-webview" in command_webview_result.get("html", ""))

        activity_desc = ExtensionDescription.from_package_json({
            "name": "activity-container",
            "publisher": "selftest",
            "version": "0.0.1",
            "contributes": {
                "commands": [
                    {
                        "command": "selftest.activity.refresh",
                        "title": "Refresh Activity",
                        "icon": "$(refresh)",
                    },
                    {
                        "command": "selftest.activity.openItem",
                        "title": "Open Activity Item",
                    },
                ],
                "menus": {
                    "view/title": [{
                        "command": "selftest.activity.refresh",
                        "when": "view == selftest.activity.tree",
                        "group": "navigation@1",
                    }],
                    "view/item/context": [{
                        "command": "selftest.activity.openItem",
                        "when": "view == selftest.activity.tree && viewItem == branch",
                        "group": "inline@1",
                    }],
                },
                "viewsContainers": {"activitybar": [{
                    "id": "selftest.activity",
                    "title": "Selftest Activity",
                }]},
                "views": {"selftest.activity": [
                    {"id": "selftest.activity.tree", "name": "Activity Tree"},
                    {"id": "selftest.activity.webview", "name": "Activity Webview", "type": "webview"},
                ]},
            },
        }, "/tmp/selftest-activity")
        api._ext_host.ext_points.process(activity_desc)
        class _ImmediateThenable:
            def __init__(self, value):
                self.value = value

            def then(self, resolve, reject=None):
                resolve(self.value)
                return self

        class _ActivityTreeProvider:
            def __init__(self):
                self._emitter = EventEmitter()

            @property
            def onDidChangeTreeData(self):
                return self._emitter.event

            def refresh(self, element=None):
                self._emitter.fire(element)

            def getChildren(self, element=None):
                if element == "node-a":
                    return _ImmediateThenable(["leaf-a"])
                return _ImmediateThenable(
                    ["node-a", "node-b"] if element is None else [])

            def getParent(self, element):
                if element == "leaf-a":
                    return _ImmediateThenable("node-a")
                return _ImmediateThenable(None)

            def getTreeItem(self, element):
                if element == "node-a":
                    return _ImmediateThenable({
                        "label": "Node A",
                        "description": "branch",
                        "tooltip": "Expandable node",
                        "resourceUri": "file:///workspace/node-a.txt",
                        "iconPath": {"id": "folder", "color": {"id": "charts.green"}},
                        "collapsibleState": 1,
                        "contextValue": "branch",
                        "checkboxState": {
                            "state": 1,
                            "tooltip": "Enabled",
                            "accessibilityInformation": {"label": "Node A enabled"},
                        },
                        "accessibilityInformation": {
                            "label": "Node A tree item",
                            "role": "treeitem",
                        },
                        "command": {
                            "command": "selftest.command.tree",
                            "title": "Open Node A",
                            "arguments": [{"from": "tree"}],
                        },
                    })
                resource_uri = (
                    {
                        "scheme": "file",
                        "path": "/workspace/node-b.txt",
                        "query": "from=test",
                    }
                    if element == "node-b" else "file:///workspace/leaf-a.txt")
                return _ImmediateThenable({
                    "label": str(element).title(),
                    "description": True,
                    "resourceUri": resource_uri,
                    "iconPath": {
                        "light": "icons/light/file.svg",
                        "dark": "icons/dark/file.svg",
                    },
                    "collapsibleState": 0,
                })

        activity_tree_provider = _ActivityTreeProvider()
        activity_command_log = []
        activity_api = api._vscode_ns.build(activity_desc)
        activity_api["commands"]["registerCommand"](
            "selftest.activity.refresh",
            lambda: activity_command_log.append(("refresh", None)) or {"refreshed": True})
        activity_api["commands"]["registerCommand"](
            "selftest.activity.openItem",
            lambda item: activity_command_log.append(("open", item)) or {"opened": str(item)})
        activity_api["window"]["registerTreeDataProvider"](
            "selftest.activity.tree", activity_tree_provider)
        activity_api["window"]["registerWebviewViewProvider"](
            "selftest.activity.webview", _CommandWebviewProvider())
        activity_tree_view = api._vscode_ns._tree_views.get("selftest.activity.tree")
        activity_selection_events = []
        activity_expand_events = []
        activity_collapse_events = []
        if activity_tree_view is not None:
            activity_tree_view.onDidChangeSelection(
                lambda evt: activity_selection_events.append(evt))
            activity_tree_view.onDidExpandElement(
                lambda evt: activity_expand_events.append(evt))
            activity_tree_view.onDidCollapseElement(
                lambda evt: activity_collapse_events.append(evt))
        activity_items = {
            item.get("id"): item
            for item in api.list_extension_activity_bar_items().get("items", [])
        }
        activity_views = {
            item.get("id"): item
            for item in activity_items.get("selftest.activity", {}).get("views", [])
        }
        _check("activity bar containers include runtime extension views",
               activity_items.get("selftest.activity", {}).get("view_count") == 2
               and activity_views.get("selftest.activity.tree", {}).get("runtimeState", {}).get("kind") == "treeView"
               and activity_views.get("selftest.activity.webview", {}).get("runtimeState", {}).get("kind") == "webviewView"
               and "command-webview" in activity_views.get("selftest.activity.webview", {}).get("runtimeState", {}).get("html", ""))
        activity_tree_nodes = activity_views.get("selftest.activity.tree", {}).get("runtimeState", {}).get("nodes", [])
        _check("activity tree thenable provider exposes structured expandable nodes",
               activity_tree_nodes
               and activity_tree_nodes[0].get("label") == "Node A"
               and activity_tree_nodes[0].get("description") == "branch"
               and activity_tree_nodes[0].get("tooltip") == "Expandable node"
               and activity_tree_nodes[0].get("resourceUri") == "file:///workspace/node-a.txt"
               and activity_tree_nodes[0].get("icon") == "$(folder)"
               and activity_tree_nodes[0].get("iconPath", {}).get("kind") == "theme"
               and activity_tree_nodes[0].get("themeIcon", {}).get("id") == "folder"
               and activity_tree_nodes[0].get("themeIcon", {}).get("color") == "charts.green"
               and activity_tree_nodes[0].get("checkbox", {}).get("isChecked") is True
               and activity_tree_nodes[0].get("checkbox", {}).get("tooltip") == "Enabled"
               and activity_tree_nodes[0].get("accessibilityInformation", {}).get("label") == "Node A tree item"
               and activity_tree_nodes[0].get("command", {}).get("command") == "selftest.command.tree"
               and activity_tree_nodes[0].get("children") == []
               and activity_tree_nodes[0].get("childrenLoaded") is False
               and activity_tree_nodes[0].get("lazyChildren") is True
               and bool(activity_tree_nodes[0].get("handle"))
               and len(activity_tree_nodes) > 1
               and activity_tree_nodes[1].get("resourceUri")
               == "file:///workspace/node-b.txt?from=test")
        _check("activity tree views expose contributed title and item actions",
               activity_views.get("selftest.activity.tree", {})
               .get("runtimeState", {}).get("titleActions", [{}])[0].get("command")
               == "selftest.activity.refresh"
               and activity_tree_nodes[0].get("actions", [{}])[0].get("command")
               == "selftest.activity.openItem")
        activity_tree_handle = activity_tree_nodes[0].get("handle", "")
        loaded_activity_children = api.load_extension_tree_children(
            "selftest.activity.tree", activity_tree_handle)
        _check("activity tree lazy loads direct children on demand",
               loaded_activity_children.get("ok") is True
               and loaded_activity_children.get("nodes", [{}])[0].get("label") == "Leaf-A"
               and loaded_activity_children.get("nodes", [{}])[0].get("description") == ""
               and loaded_activity_children.get("nodes", [{}])[0].get("descriptionIsDerived") is True
               and loaded_activity_children.get("nodes", [{}])[0].get("resourceUri") == "file:///workspace/leaf-a.txt"
               and loaded_activity_children.get("nodes", [{}])[0].get("iconPath", {}).get("kind") == "themedPath"
               and loaded_activity_children.get("nodes", [{}])[0].get("icon") == "$(file)"
               and loaded_activity_children.get("nodes", [{}])[0].get("childrenLoaded") is True
               and loaded_activity_children.get("nodes", [{}])[0].get("actions", []) == [])
        expanded_activity = api.set_extension_tree_item_expanded(
            "selftest.activity.tree", activity_tree_handle, True)
        collapsed_activity = api.set_extension_tree_item_expanded(
            "selftest.activity.tree", activity_tree_handle, False)
        _check("activity tree expand and collapse events reach runtime TreeView",
               expanded_activity.get("ok") is True
               and collapsed_activity.get("ok") is True
               and activity_expand_events[-1].get("element") == "node-a"
               and activity_collapse_events[-1].get("element") == "node-a")
        selected_tree = api.select_extension_tree_item(
            "selftest.activity.tree", activity_tree_handle)
        _check("activity tree selection updates runtime TreeView event state",
               selected_tree.get("ok") is True
               and activity_selection_events
               and activity_selection_events[-1].get("selection") == ["node-a"]
               and selected_tree.get("runtimeState", {}).get("nodes", [{}])[0].get("selected") is True)
        item_action = api.execute_extension_tree_item_action(
            "selftest.activity.tree",
            activity_tree_handle,
            "selftest.activity.openItem")
        _check("activity tree item context action receives selected element",
               item_action.get("ok") is True
               and item_action.get("opened") == "node-a"
               and activity_command_log[-1] == ("open", "node-a"))
        if activity_tree_view is not None:
            activity_tree_view.reveal("leaf-a")
        revealed_activity_views = {
            view.get("id"): view
            for item in api.list_extension_activity_bar_items().get("items", [])
            if item.get("id") == "selftest.activity"
            for view in item.get("views", [])
        }
        revealed_activity_node = revealed_activity_views.get(
            "selftest.activity.tree", {}).get(
                "runtimeState", {}).get("nodes", [{}])[0]
        revealed_activity_child = revealed_activity_node.get("children", [{}])[0]
        _check("activity tree reveal expands thenable provider parent chain",
               revealed_activity_node.get("revealAncestor") is True
               and revealed_activity_node.get("collapsibleState") == 2
               and revealed_activity_node.get("childrenLoaded") is True
               and revealed_activity_child.get("label") == "Leaf-A"
               and revealed_activity_child.get("revealed") is True
               and revealed_activity_child.get("selected") is True
               and revealed_activity_child.get("revealVersion", 0) >= 1)
        before_activity_version = activity_views.get(
            "selftest.activity.tree", {}).get("runtimeState", {}).get("refreshVersion", 0)
        activity_tree_provider.refresh("node-a")
        refreshed_activity_views = {
            view.get("id"): view
            for item in api.list_extension_activity_bar_items().get("items", [])
            if item.get("id") == "selftest.activity"
            for view in item.get("views", [])
        }
        _check("activity tree refresh events advance runtime snapshot version",
               refreshed_activity_views.get("selftest.activity.tree", {})
               .get("runtimeState", {}).get("refreshVersion", 0)
               > before_activity_version)

        contribs = api.get_extension_contributions().get("contributions", {})
        command_items = {
            item.get("command"): item for item in contribs.get("commands", [])
            if item.get("command")
        }
        decorated_views = {}
        for location, items in contribs.get("views", {}).items():
            for item in items:
                decorated_views[item.get("id")] = item
        _check("contributions expose activation metadata for commands",
               command_items.get("selftest.command.tool", {}).get("activation", {}).get("effectiveEvents")
               == ["onCommand:selftest.command.tool"])
        _check("contributions expose runtime-backed view metadata",
               decorated_views.get("selftest.command.tree.view", {}).get("runtimeState", {}).get("kind") == "treeView"
               and decorated_views.get("selftest.command.webview.view", {}).get("runtimeState", {}).get("kind") == "webviewView")

        provider = ChatProviderDef(
            id="selftest-provider",
            name="Selftest Provider",
            provider_type="openai",
            api_key="test-openai",
            model="gpt-4o",
        )
        ctrl = api._create_provider_controller(provider)
        api._provider_controllers[provider.id] = ctrl
        api.register_mcp_tools(
            "selftest_tools",
            [{
                "name": "echo",
                "description": "Echo value",
                "inputSchema": {
                    "type": "object",
                    "properties": {"value": {"type": "string"}},
                    "required": ["value"],
                },
            }],
            {"echo": lambda value=None, **kw: {"value": value or kw.get("value")}},
        )
        provider_tool_names = [
            item.get("function", {}).get("name")
            for item in (ctrl.extra_tools or [])
        ]
        _check("provider controller receives plugin tool schema",
               "mcp_selftest_tools_echo" in provider_tool_names)
        dispatch_result = ctrl.mcp_dispatch(
            "mcp_selftest_tools_echo", {"value": "pong"}) if ctrl.mcp_dispatch else ""
        dispatch_data = json.loads(dispatch_result) if isinstance(dispatch_result, str) else dispatch_result
        _check("provider controller plugin tool dispatch works",
               dispatch_data.get("value") == "pong")
        missing_provider = api._mcp.register_internal(
            "selftest_missing_handler",
            [{"name": "needs_handler", "description": "Missing handler"}],
        )
        missing_data = json.loads(missing_provider.call_tool("needs_handler", {}))
        _check("internal MCP missing handler is explicit error",
               "no registered handler" in missing_data.get("error", ""))
    finally:
        extension_host_module._host = previous_host
        if language_tmp:
            shutil.rmtree(language_tmp, ignore_errors=True)
        if settings_tmp:
            shutil.rmtree(settings_tmp, ignore_errors=True)


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

        class _DictProvider:
            def create_session(self, scopes=None, options=None):
                return {
                    "id": "dict-session",
                    "accessToken": "dict-token",
                    "account": {"id": "dict-user", "label": "Dict User"},
                    "scopes": list(scopes or []),
                }

        svc.register_provider("dict-provider", "Dict Provider", _DictProvider())
        dict_session = svc.get_session(
            "dict-provider", ["repo"], {"createIfNone": True})
        _check("dict provider session normalized",
               dict_session is not None
               and dict_session.access_token == "dict-token"
               and dict_session.account_label == "Dict User")

        # Remove
        ok = svc.remove_session("github", s.id)
        _check("remove", ok and len(svc.list_sessions("github")) == 0)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_claude_proxy() -> None:
    print("── Claude Proxy ──")
    from ai_editor.claude_proxy import ClaudeProxy
    from ai_editor.claude_proxy import _ProxyHandler
    from ai_editor.llm_engine import ToolCall

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

    tool_schema = _ProxyHandler._tools_schema([
        {"name": "readFile", "description": "Read", "input_schema": {"type": "object", "properties": {}}}
    ])
    converted_msgs = _ProxyHandler._anthropic_messages_to_engine(
        "system text",
        [{
            "role": "user",
            "content": [
                {"type": "text", "text": "hello"},
                {"type": "tool_result", "tool_use_id": "tool-1", "content": "done"},
            ],
        }],
    )
    response_blocks = _ProxyHandler._anthropic_content_from_response(type("Resp", (), {
        "content": "done",
        "tool_calls": [ToolCall(id="tool-2", name="writeFile", arguments='{"path":"a.txt"}')],
    })())
    _check("claude proxy converts tool schemas and messages",
           tool_schema and tool_schema[0].get("function", {}).get("name") == "readFile"
           and converted_msgs[0].get("role") == "system"
           and any(msg.get("role") == "tool" for msg in converted_msgs)
           and any(block.get("type") == "tool_use" for block in response_blocks))

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

        r = reg.save_custom(AgentDef(id="../escape", name="Bad"), workspace_root=tmpdir)
        _check("agent save blocks path traversal", r.get("ok") is False)

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

        r = reg.save_custom(WorkflowDef(id="../escape", name="Bad"), workspace_root=tmpdir)
        _check("workflow save blocks path traversal", r.get("ok") is False)

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
        except Exception as cleanup_exc:
            print(f"[Selftest] Tk cleanup failed: {cleanup_exc}")


def main() -> None:
    print("=" * 50)
    print("SAO AI Editor — Selftest")
    print("=" * 50)

    test_imports()
    test_tool_registry()
    test_mcp_client()
    test_llm_engine()
    test_conversation()
    test_chat_controller_tool_loop()
    test_history()
    test_bridge()
    test_app_settings_parity()
    test_phase1_ai_editor_regressions()
    test_instructions()
    test_scopes()
    test_extension_host()
    test_vscode_api()
    test_app_extension_runtime_support()
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
