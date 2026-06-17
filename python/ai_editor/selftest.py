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

    chat_perms = effective_permissions("chat")
    _check("chat: readFile disabled", chat_perms.get("readFile") == "disabled")
    _check("chat: engine allowed", chat_perms.get("engine") == "allowed")

    edit_perms = effective_permissions("edit")
    _check("edit: readFile allowed", edit_perms.get("readFile") == "allowed")
    _check("edit: editFile confirm", edit_perms.get("editFile") == "confirm")

    agent_perms = effective_permissions("agent")
    _check("agent: editFile allowed", agent_perms.get("editFile") == "allowed")

    # Overrides
    custom = effective_permissions("edit", {"readFile": "disabled"})
    _check("override readFile", custom["readFile"] == "disabled")

    # tool_permission helper
    _check("tool_permission chat/readFile",
           tool_permission("chat", "readFile") == "disabled")
    _check("tool_permission edit/readFile",
           tool_permission("edit", "readFile") == "allowed")
    _check("tool_permission unknown tool defaults allowed",
           tool_permission("edit", "some_unknown_tool") == "allowed")


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
        custom = AgentDef(id="test-agent", name="Test Agent",
                          description="For testing", system_prompt="Be helpful.")
        r = reg.save_custom(custom, workspace_root=tmpdir)
        _check("save custom", r.get("ok") is True)
        _check("custom in registry", reg.get("test-agent") is not None)

        # Load custom from disk
        reg2 = AgentRegistry()
        reg2.load_custom(workspace_root=tmpdir)
        _check("loaded from disk", reg2.get("test-agent") is not None)

        # Delete custom
        r = reg.delete_custom("test-agent", workspace_root=tmpdir)
        _check("delete custom", r.get("ok") is True)
        _check("gone after delete", reg.get("test-agent") is None)

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
        custom = WorkflowDef(
            id="test-wf", name="Test WF", description="Testing",
            steps=[WorkflowStep(prompt="Echo: {{input}}", output_var="out", label="Echo")],
        )
        r = reg.save_custom(custom, workspace_root=tmpdir)
        _check("save custom wf", r.get("ok") is True)

        # Load from disk
        reg2 = WorkflowRegistry()
        reg2.load_custom(workspace_root=tmpdir)
        _check("loaded wf from disk", reg2.get("test-wf") is not None)
        _check("steps preserved", len(reg2.get("test-wf").steps) == 1)

        # Delete custom
        r = reg.delete_custom("test-wf", workspace_root=tmpdir)
        _check("delete custom wf", r.get("ok") is True)

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
