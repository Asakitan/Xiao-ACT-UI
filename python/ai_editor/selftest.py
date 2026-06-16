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
        from gui_modules.sao_gui_ai_editor import AIEditorPanel
        _check("sao_gui_ai_editor", True)
    except Exception as e:
        _check("sao_gui_ai_editor", False, str(e))


class _FakeGui:
    settings = None
    _game_state = {"uid": 12345, "name": "TestPlayer", "level": 60, "profession": "Swordsman"}
    _rows = {}
    _dps_tracker = None
    _encounter_manager = None
    _boss_raid_engine = None
    _trigger_engine = None
    _auto_key_engine = None
    _plugin_manager = None
    _packet_bridge = None
    _mem_bridge = None
    _start_time = 0.0


def test_tool_registry() -> None:
    print("── Tool Registry ──")
    from ai_editor.tool_registry import ToolRegistry
    from ai_editor.engine_tools import register_engine_tools

    reg = ToolRegistry()
    register_engine_tools(reg, _FakeGui())

    tools = reg.list_tools()
    _check(f"tools registered: {len(tools)}", len(tools) >= 20)

    cats = reg.categories()
    _check(f"categories: {cats}", len(cats) >= 7)

    schemas = reg.to_openai_tools()
    _check(f"OpenAI schemas: {len(schemas)}", len(schemas) == len(tools))

    # Execute a tool
    result = reg.execute("get_game_state", "{}")
    data = json.loads(result)
    _check(f"get_game_state exec: uid={data.get('uid')}", data.get("uid") == 12345)

    result = reg.execute("get_system_info", "{}")
    data = json.loads(result)
    _check(f"get_system_info exec", "version" in data)

    result = reg.execute("eval_python", '{"expression": "2 + 2"}')
    data = json.loads(result)
    _check(f"eval_python: 2+2={data.get('result')}", data.get("result") == 4)

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
    _check(f"list_tools: {len(result.get('tools', []))}", len(result.get("tools", [])) >= 20)

    result = bridge.handle_command("ai_editor_list_history", {})
    _check("list_history", "entries" in result)

    result = bridge.handle_command("unknown_cmd", {})
    _check("unknown cmd", "error" in result)


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
        _check("Slash commands defined", len(panel._SLASH_COMMANDS) >= 10)

        # Ensure engine init
        panel._ensure_engine()
        _check("Engine initialized", panel._engine is not None)
        _check("Registry initialized", panel._registry is not None)
        _check("Controller initialized", panel._controller is not None)

        # Test slash command execution
        panel._execute_slash_command("/state")
        _check("/state executed", True)

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

        panel._render_tool_call("get_game_state", '{"key": "value"}')
        _check("Tool call rendered", True)

        panel._render_tool_result("get_game_state", '{"uid": 12345}')
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
