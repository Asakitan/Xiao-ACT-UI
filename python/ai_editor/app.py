"""SAO AI Editor — standalone pywebview GUI application.

Launch:
    python -m ai_editor.app            # standalone
    python -m ai_editor.app --attach   # attached to running SAO instance

From SAO menu, ``_toggle_ai_editor_panel`` calls ``launch()`` which opens
the pywebview window in a background thread if not already running.
"""

from __future__ import annotations

import json
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
                        print(f"[MCP] Connected: {cfg.id} ({len([t for c in [self._mcp._clients[cfg.id]] for t in c.tools])} tools)")
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
        self._controller.on_error = self._on_error
        self._controller.on_idle = self._on_idle

        # Inject MCP tools into controller
        if self._mcp:
            self._controller.extra_tools = self._mcp.to_openai_tools()
            self._controller.mcp_dispatch = lambda name, args: self._mcp.call_tool(
                name, json.loads(args) if isinstance(args, str) else args
            )

        self._pending_confirm: Dict[str, threading.Event] = {}
        self._confirm_results: Dict[str, bool] = {}

    def _default_system_prompt(self) -> str:
        cats = ""
        if self._registry:
            for cat in self._registry.categories():
                names = [t.name for t in self._registry.list_tools(cat)]
                cats += f"\n- {cat}: {', '.join(names)}"
        return (
            "你是 SAO ACT 的AI助手。你可以通过 tool call 访问游戏引擎底层接口。\n"
            f"可用工具:{cats}\n"
            "代码输出用 markdown 代码块。优先使用用户的语言回答。"
        )

    # ── Config ──

    def _load_config_obj(self) -> ProviderConfig:
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
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

    # ── JS-callable methods (window.pywebview.api.*) ──

    def load_config(self) -> Dict:
        cfg = self._load_config_obj()
        return {
            "provider": cfg.provider, "api_key": cfg.api_key,
            "base_url": cfg.base_url, "model": cfg.model or cfg.effective_model,
            "temperature": cfg.temperature, "max_tokens": cfg.max_tokens,
            "system_prompt": cfg.system_prompt,
        }

    def save_config(self, data: Dict) -> Dict:
        settings = getattr(self._gui_ref, 'settings', None) if self._gui_ref else None
        if settings:
            settings.set("ai_editor", data)
            try:
                settings.save()
            except Exception:
                pass
        if self._engine:
            for k, v in data.items():
                if hasattr(self._engine.config, k):
                    setattr(self._engine.config, k, v)
        return {"ok": True}

    def send_message(self, text: str, config: Optional[Dict] = None) -> Dict:
        if not text or not text.strip():
            return {"error": "Empty message"}
        self._ensure_engine()
        if config:
            if config.get("provider"):
                self._engine.config.provider = config["provider"]
            if config.get("model"):
                self._engine.config.model = config["model"]
        self._controller.send(text.strip())
        return {"ok": True}

    def cancel(self) -> Dict:
        if self._controller:
            self._controller.cancel()
        return {"ok": True}

    def new_chat(self) -> Dict:
        if self._controller:
            sp = self._engine.config.system_prompt if self._engine else ""
            self._controller.new_conversation(sp or self._default_system_prompt())
        return {"ok": True}

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
            for t in self._mcp.all_tools():
                tools.append({
                    "name": f"mcp_{t.server_id}_{t.name}",
                    "description": f"[MCP:{t.server_id}] {t.description}",
                    "category": f"mcp:{t.server_id}",
                    "requires_confirm": False,
                    "parameters": t.input_schema,
                })
        return {"tools": tools}

    def execute_tool(self, name: str, arguments: str = "{}") -> str:
        self._ensure_engine()
        if name.startswith("mcp_") and self._mcp:
            args = json.loads(arguments) if isinstance(arguments, str) else arguments
            return self._mcp.call_tool(name, args)
        return self._registry.execute(name, arguments)

    def list_mcp_servers(self) -> Dict:
        self._ensure_engine()
        if not self._mcp:
            return {"servers": []}
        return {"servers": self._mcp.list_servers()}

    def add_mcp_server(self, config: Dict) -> Dict:
        self._ensure_engine()
        if not self._mcp:
            from ai_editor.mcp_client import McpManager
            self._mcp = McpManager()
        from ai_editor.mcp_client import McpServerConfig
        cfg = McpServerConfig(
            id=config.get("id", ""),
            name=config.get("name", config.get("id", "")),
            transport=config.get("transport", "stdio"),
            command=config.get("command", ""),
            args=config.get("args", []),
            url=config.get("url", ""),
        )
        ok = self._mcp.add_server(cfg)
        return {"ok": ok, "id": cfg.id, "tools": len(self._mcp._clients.get(cfg.id, type('',(),{'tools':[]})()).tools) if ok else 0}

    def remove_mcp_server(self, server_id: str) -> Dict:
        if self._mcp:
            self._mcp.remove_server(server_id)
        return {"ok": True}

    def test_connection(self, cfg: Dict) -> Dict:
        self._ensure_engine()
        test_cfg = ProviderConfig(
            provider=cfg.get("provider", "openai"),
            api_key=cfg.get("api_key", ""),
            base_url=cfg.get("base_url", ""),
            model=cfg.get("model", ""),
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

    def install_extension(self, ext_id: str, vsix_url: str = "") -> Dict:
        """Install an extension from the marketplace."""
        try:
            from ai_editor.extensions import install_extension
            return install_extension(ext_id, vsix_url)
        except Exception as exc:
            return {"error": str(exc)}

    def uninstall_extension(self, ext_id: str) -> Dict:
        """Uninstall an extension."""
        try:
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
        """Get editor content via JS eval."""
        if not self._window:
            return {"content": "", "language": "plaintext"}
        try:
            content = self._window.evaluate_js("document.getElementById('editor-text').value")
            lang = self._window.evaluate_js("editorLang")
            return {"content": content or "", "language": lang or "plaintext"}
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
        self._emit("stream_delta", {"content": text})

    def _on_thinking_delta(self, msg: ChatMessage, text: str) -> None:
        self._emit("thinking_delta", {"content": text})

    def _on_stream_end(self, msg: ChatMessage) -> None:
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

    def _on_tool_end(self, call_id: str, result: str) -> None:
        self._emit("tool_end", {"id": call_id, "result": result})

    def _on_error(self, error: str) -> None:
        self._emit("error", {"error": error})

    def _on_idle(self) -> None:
        self._emit("idle", {})


class _DummyGui:
    """Fallback when launched standalone without SAO instance."""
    settings = None
    _game_state = {"uid": 0, "name": "(standalone)", "level": 0}
    _rows = {}
    _dps_tracker = None
    _encounter_manager = None
    _boss_raid_engine = None
    _trigger_engine = None
    _auto_key_engine = None
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

    If *blocking* is False (default), opens in a background thread and returns
    immediately.  Safe to call multiple times — if already open, focuses the
    existing window.
    """
    global _running_window, _running_thread

    if _running_window is not None:
        try:
            _running_window.show()
            return
        except Exception:
            _running_window = None

    api = AIEditorAPI(gui_ref)

    def _run():
        global _running_window
        import webview
        html_file = _html_path()
        if not os.path.isfile(html_file):
            print(f"[AIEditor] HTML not found: {html_file}")
            return
        url = f"file:///{html_file.replace(os.sep, '/')}"
        window = webview.create_window(
            "SAO AI Editor",
            url=url,
            width=1200,
            height=800,
            min_size=(800, 500),
            js_api=api,
            frameless=False,
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

    if blocking:
        _run()
    else:
        _running_thread = threading.Thread(target=_run, daemon=True)
        _running_thread.start()


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    launch(blocking=True)
