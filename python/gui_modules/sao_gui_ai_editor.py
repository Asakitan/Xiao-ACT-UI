"""SAO AI Editor — VSCode-style LLM chat + tool-calling panel.

Standalone Tk Toplevel window opened from the SAO menu.  Features:
* Multi-provider LLM chat with streaming
* Tool calling (engine APIs exposed as LLM functions)
* Code-block rendering with syntax tags
* Collapsible tool-call result panels
* API-key / model / provider settings dialog
* Conversation history sidebar
"""

from __future__ import annotations

import json
import re
import textwrap
import threading
import tkinter as tk
import tkinter.font as tkfont
import time
from tkinter import ttk
from typing import Any, Callable, Dict, List, Optional, Tuple

from gui_modules.sao_panel_ui import (
    _SAO_PANEL_BG,
    _SAO_PANEL_HEADER_BG,
    _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_BORDER,
    _SAO_PANEL_ACCENT,
    _sao_panel_header,
)

# ---------------------------------------------------------------------------
# Color palette (VSCode-inspired dark theme)
# ---------------------------------------------------------------------------

_C = {
    "bg":            "#1e1e1e",
    "bg_secondary":  "#252526",
    "bg_input":      "#2d2d2d",
    "bg_sidebar":    "#181818",
    "bg_code":       "#1a1a2e",
    "bg_tool":       "#1e2a1e",
    "bg_tool_result": "#162016",
    "bg_user":       "#264f78",
    "bg_assistant":  "#2d2d2d",
    "bg_error":      "#5a1d1d",
    "fg":            "#cccccc",
    "fg_dim":        "#808080",
    "fg_bright":     "#e0e0e0",
    "fg_accent":     "#4fc1ff",
    "fg_success":    "#4ec9b0",
    "fg_error":      "#f14c4c",
    "fg_warning":    "#cca700",
    "fg_tool_name":  "#dcdcaa",
    "fg_code":       "#d4d4d4",
    "fg_keyword":    "#569cd6",
    "fg_string":     "#ce9178",
    "fg_number":     "#b5cea8",
    "fg_comment":    "#6a9955",
    "border":        "#3c3c3c",
    "border_focus":  "#007acc",
    "scrollbar":     "#424242",
    "scrollbar_active": "#686868",
    "button":        "#0e639c",
    "button_hover":  "#1177bb",
    "button_danger": "#c53434",
    "selection":     "#264f78",
}

# ---------------------------------------------------------------------------
# Fonts
# ---------------------------------------------------------------------------

def _get_fonts() -> Dict[str, tkfont.Font]:
    mono = "Cascadia Code" if _font_exists("Cascadia Code") else "Consolas"
    sans = "Segoe UI" if _font_exists("Segoe UI") else "TkDefaultFont"
    return {
        "body":     tkfont.Font(family=sans, size=10),
        "body_b":   tkfont.Font(family=sans, size=10, weight="bold"),
        "small":    tkfont.Font(family=sans, size=9),
        "mono":     tkfont.Font(family=mono, size=10),
        "mono_sm":  tkfont.Font(family=mono, size=9),
        "heading":  tkfont.Font(family=sans, size=11, weight="bold"),
        "title":    tkfont.Font(family=sans, size=12, weight="bold"),
    }


def _font_exists(name: str) -> bool:
    try:
        return name.lower() in [f.lower() for f in tkfont.families()]
    except Exception:
        return False


# =====================================================================
# Main Panel
# =====================================================================

class AIEditorPanel:
    """Standalone Tk window for AI-assisted editing."""

    def __init__(self, root: tk.Tk, gui_ref: Any) -> None:
        self.root = root
        self._gui_ref = gui_ref
        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._fonts: Dict[str, tkfont.Font] = {}

        # Chat engine (lazy-init)
        self._controller = None
        self._engine = None
        self._registry = None

        # UI refs
        self._chat_text: Optional[tk.Text] = None
        self._input_text: Optional[tk.Text] = None
        self._status_var: Optional[tk.StringVar] = None
        self._model_var: Optional[tk.StringVar] = None
        self._provider_var: Optional[tk.StringVar] = None

        # Streaming state
        self._stream_tag_counter = 0
        self._current_stream_end = None

        # Input history
        self._input_history: List[str] = []
        self._history_idx = -1

        # Sidebar & chat frame ref
        self._sidebar: Optional[tk.Frame] = None
        self._sidebar_visible = False
        self._sidebar_list_frame: Optional[tk.Frame] = None
        self._chat_frame: Optional[tk.Frame] = None

        # Slash command popup
        self._slash_popup: Optional[tk.Toplevel] = None

        # Drag
        self._drag_ox = 0
        self._drag_oy = 0

    # -- Public interface --

    def is_visible(self) -> bool:
        return bool(self._visible and self._win and self._win.winfo_exists())

    def show(self) -> None:
        if self._win is None or not self._win.winfo_exists():
            self._build()
        self._visible = True
        self._win.deiconify()
        self._win.lift()

    def hide(self) -> None:
        if self._win is not None:
            self._win.withdraw()
        self._visible = False

    def destroy(self) -> None:
        if self._controller:
            self._controller.cancel()
        self._hide_slash_popup()
        if self._slash_popup:
            try:
                self._slash_popup.destroy()
            except Exception:
                pass
            self._slash_popup = None
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
            self._win = None
        self._visible = False

    # -- Lazy engine init --

    def _ensure_engine(self) -> None:
        if self._controller is not None:
            return
        from ai_editor.llm_engine import LLMEngine, ProviderConfig
        from ai_editor.tool_registry import ToolRegistry
        from ai_editor.engine_tools import register_engine_tools
        from ai_editor.chat_state import ChatController, Conversation

        config = self._load_config()
        self._engine = LLMEngine(config)
        self._registry = ToolRegistry()
        register_engine_tools(self._registry, self._gui_ref)

        system_prompt = config.system_prompt or self._default_system_prompt()
        conv = Conversation(system_prompt=system_prompt)
        self._controller = ChatController(self._engine, self._registry, conv)

        self._controller.on_message_added = self._on_message_added
        self._controller.on_stream_delta = self._on_stream_delta
        self._controller.on_stream_end = self._on_stream_end
        self._controller.on_tool_start = self._on_tool_start
        self._controller.on_tool_end = self._on_tool_end
        self._controller.on_error = self._on_error
        self._controller.on_idle = self._on_idle

    def _default_system_prompt(self) -> str:
        from ai_editor.prompts import get_system_prompt
        return get_system_prompt()

    # -- Config persistence --

    def _load_config(self):
        from ai_editor.llm_engine import ProviderConfig
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

    def _save_config(self, config) -> None:
        settings = getattr(self._gui_ref, 'settings', None)
        if not settings:
            return
        settings.set("ai_editor", {
            "provider": config.provider,
            "api_key": config.api_key,
            "base_url": config.base_url,
            "model": config.model,
            "temperature": config.temperature,
            "max_tokens": config.max_tokens,
            "system_prompt": config.system_prompt,
        })
        try:
            settings.save()
        except Exception:
            pass

    # ================================================================
    # Build UI
    # ================================================================

    def _build(self) -> None:
        self._fonts = _get_fonts()

        from render.tk_mirror import SaoToplevel
        win = SaoToplevel(self.root, mirror_name='ai_editor')
        self._win = win
        win.title("SAO AI Editor (Classic)")
        win.geometry("960x700+200+100")
        win.minsize(680, 480)
        win.configure(bg=_C["bg"])
        win.protocol("WM_DELETE_WINDOW", self.hide)

        # ── Title bar ──
        header = tk.Frame(win, bg=_C["bg_secondary"], height=36)
        header.pack(fill="x")
        header.pack_propagate(False)

        title_lbl = tk.Label(
            header, text="✦ SAO AI Editor", font=self._fonts["heading"],
            bg=_C["bg_secondary"], fg=_C["fg_accent"], anchor="w", padx=12,
        )
        title_lbl.pack(side="left", fill="y")

        # Provider / Model selectors in header
        self._provider_var = tk.StringVar(value="openai")
        self._model_var = tk.StringVar(value="")

        provider_menu = ttk.Combobox(
            header, textvariable=self._provider_var, width=10, state="readonly",
            values=["openai", "anthropic", "deepseek", "ollama", "custom"],
        )
        provider_menu.pack(side="left", padx=(20, 4), pady=6)
        provider_menu.bind("<<ComboboxSelected>>", lambda _: self._on_provider_change())

        self._model_entry = tk.Entry(
            header, textvariable=self._model_var, width=22,
            bg=_C["bg_input"], fg=_C["fg"], insertbackground=_C["fg"],
            relief="flat", font=self._fonts["small"],
        )
        self._model_entry.pack(side="left", padx=4, pady=6)

        # Settings button
        settings_btn = tk.Label(
            header, text=" ⚙ ", font=self._fonts["body_b"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2",
        )
        settings_btn.pack(side="right", padx=4)
        settings_btn.bind("<Button-1>", lambda _: self._show_settings_dialog())

        # Close button
        close_btn = tk.Label(
            header, text=" ✕ ", font=self._fonts["body_b"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2",
        )
        close_btn.pack(side="right", padx=4)
        close_btn.bind("<Button-1>", lambda _: self.hide())

        # Tools button
        tools_btn = tk.Label(
            header, text=" 🔧 ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2",
        )
        tools_btn.pack(side="right", padx=4)
        tools_btn.bind("<Button-1>", lambda _: self._show_tools_panel())

        # New chat button
        new_btn = tk.Label(
            header, text=" ✚ New ", font=self._fonts["small"],
            bg=_C["bg_secondary"], fg=_C["fg_accent"], cursor="hand2",
        )
        new_btn.pack(side="right", padx=4)
        new_btn.bind("<Button-1>", lambda _: self._new_chat())

        # Header has several interactive buttons (settings/close/tools/new),
        # so it can't use the shared `_bind_panel_drag` recursive helper
        # (which only excludes a single widget from the drag binding — it
        # would silently break the other buttons). Bind drag directly to
        # just the header background and the title label instead; the old
        # `_bind_panel_drag(win, header)` call bound drag to the *entire*
        # window body (everything except `header`), making every click in
        # the chat/sidebar area move the window instead of reaching it.
        _drag_state: Dict[str, int] = {}

        def _drag_start(event):
            _drag_state['x'] = event.x_root
            _drag_state['y'] = event.y_root

        def _drag_move(event):
            try:
                dx = event.x_root - _drag_state['x']
                dy = event.y_root - _drag_state['y']
                _drag_state['x'] = event.x_root
                _drag_state['y'] = event.y_root
                win.geometry(f'+{win.winfo_x() + dx}+{win.winfo_y() + dy}')
            except Exception:
                pass

        header.bind('<Button-1>', _drag_start)
        header.bind('<B1-Motion>', _drag_move)
        title_lbl.bind('<Button-1>', _drag_start)
        title_lbl.bind('<B1-Motion>', _drag_move)

        # History toggle button in header
        history_btn = tk.Label(
            header, text=" ☰ ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2",
        )
        history_btn.pack(side="right", padx=4)
        history_btn.bind("<Button-1>", lambda _: self._toggle_sidebar())

        # ── Main area: sidebar + chat ──
        main_pane = tk.Frame(win, bg=_C["bg"])
        main_pane.pack(fill="both", expand=True)

        # ── Sidebar (history, initially hidden) ──
        self._sidebar = tk.Frame(main_pane, bg=_C["bg_sidebar"], width=220)
        self._sidebar_visible = False
        self._sidebar_list = None

        sidebar_header = tk.Label(
            self._sidebar, text="History", font=self._fonts["body_b"],
            bg=_C["bg_sidebar"], fg=_C["fg_accent"], anchor="w", padx=12, pady=6,
        )
        sidebar_header.pack(fill="x")

        self._sidebar_list_frame = tk.Frame(self._sidebar, bg=_C["bg_sidebar"])
        self._sidebar_list_frame.pack(fill="both", expand=True)

        # ── Chat area ──
        chat_frame = tk.Frame(main_pane, bg=_C["bg"])
        chat_frame.pack(side="left", fill="both", expand=True)
        self._chat_frame = chat_frame

        # Chat messages (read-only Text widget)
        self._chat_text = tk.Text(
            chat_frame, bg=_C["bg"], fg=_C["fg"], font=self._fonts["body"],
            wrap="word", state="disabled", cursor="arrow",
            relief="flat", bd=0, padx=16, pady=8,
            selectbackground=_C["selection"], selectforeground=_C["fg_bright"],
            spacing1=2, spacing3=2,
        )
        chat_scroll = tk.Scrollbar(
            chat_frame, command=self._chat_text.yview,
            bg=_C["scrollbar"], activebackground=_C["scrollbar_active"],
            troughcolor=_C["bg"], relief="flat", bd=0, width=8,
        )
        self._chat_text.configure(yscrollcommand=chat_scroll.set)
        chat_scroll.pack(side="right", fill="y")
        self._chat_text.pack(side="left", fill="both", expand=True)

        self._setup_chat_tags()

        # ── Separator ──
        tk.Frame(win, bg=_C["border"], height=1).pack(fill="x")

        # ── Input area ──
        input_frame = tk.Frame(win, bg=_C["bg_input"])
        input_frame.pack(fill="x")

        # Input toolbar
        input_toolbar = tk.Frame(input_frame, bg=_C["bg_input"])
        input_toolbar.pack(fill="x", padx=12, pady=(6, 0))

        hint_lbl = tk.Label(
            input_toolbar, text="Enter 发送 · Shift+Enter 换行 · / 命令 · Up/Down 历史",
            font=self._fonts["small"], bg=_C["bg_input"], fg=_C["fg_dim"],
        )
        hint_lbl.pack(side="left")

        # Input text
        self._input_text = tk.Text(
            input_frame, bg=_C["bg_input"], fg=_C["fg"], font=self._fonts["mono"],
            wrap="word", height=4, relief="flat", bd=0,
            insertbackground=_C["fg_accent"], padx=12, pady=6,
            selectbackground=_C["selection"], selectforeground=_C["fg_bright"],
        )
        self._input_text.pack(fill="x", padx=4, pady=(0, 4))
        self._input_text.bind("<Return>", self._on_enter)
        self._input_text.bind("<Shift-Return>", lambda e: None)  # allow newline
        self._input_text.bind("<Control-l>", lambda _: self._clear_chat())
        self._input_text.bind("<Up>", self._on_history_up)
        self._input_text.bind("<Down>", self._on_history_down)
        self._input_text.bind("<KeyRelease>", self._on_input_key_release)

        # Slash command popup (lazy)
        self._slash_popup: Optional[tk.Toplevel] = None

        # Button bar
        btn_bar = tk.Frame(input_frame, bg=_C["bg_input"])
        btn_bar.pack(fill="x", padx=12, pady=(0, 6))

        send_btn = tk.Label(
            btn_bar, text="  ▶ Send  ", font=self._fonts["body_b"],
            bg=_C["button"], fg="#ffffff", cursor="hand2", padx=8, pady=2,
        )
        send_btn.pack(side="left")
        send_btn.bind("<Button-1>", lambda _: self._do_send())
        send_btn.bind("<Enter>", lambda e: e.widget.configure(bg=_C["button_hover"]))
        send_btn.bind("<Leave>", lambda e: e.widget.configure(bg=_C["button"]))

        cancel_btn = tk.Label(
            btn_bar, text="  ■ Stop  ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2", padx=8, pady=2,
        )
        cancel_btn.pack(side="left", padx=6)
        cancel_btn.bind("<Button-1>", lambda _: self._do_cancel())
        self._cancel_btn = cancel_btn

        clear_btn = tk.Label(
            btn_bar, text="  🗑 Clear  ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2", padx=8, pady=2,
        )
        clear_btn.pack(side="left")
        clear_btn.bind("<Button-1>", lambda _: self._clear_chat())

        export_btn = tk.Label(
            btn_bar, text="  📋 Export  ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2", padx=8, pady=2,
        )
        export_btn.pack(side="left", padx=6)
        export_btn.bind("<Button-1>", lambda _: self._export_chat())

        # Token counter
        self._token_lbl = tk.Label(
            btn_bar, text="0 tokens", font=self._fonts["small"],
            bg=_C["bg_input"], fg=_C["fg_dim"],
        )
        self._token_lbl.pack(side="right")

        # ── Status bar ──
        status_frame = tk.Frame(win, bg=_C["bg_secondary"], height=24)
        status_frame.pack(fill="x")
        status_frame.pack_propagate(False)

        self._status_var = tk.StringVar(value="Ready")
        status_lbl = tk.Label(
            status_frame, textvariable=self._status_var, font=self._fonts["small"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], anchor="w", padx=12,
        )
        status_lbl.pack(side="left", fill="y")

        # Load saved config into UI
        self._sync_config_to_ui()

        # Welcome message
        self._append_system_message(
            "✦ SAO AI Editor\n"
            "多模型LLM编辑器, 支持 OpenAI / Claude / DeepSeek / Ollama\n"
            "运行时工具已就绪: 内存状态 · 插件列表 · 设置 · Python执行\n\n"
            "先在 ⚙ Settings 中配置 API Key, 然后开始对话。"
        )

    # ================================================================
    # Chat tags (syntax highlighting)
    # ================================================================

    def _setup_chat_tags(self) -> None:
        ct = self._chat_text
        if not ct:
            return
        ct.tag_configure("user_header", foreground=_C["fg_accent"], font=self._fonts["body_b"],
                         spacing1=12)
        ct.tag_configure("assistant_header", foreground=_C["fg_success"], font=self._fonts["body_b"],
                         spacing1=12)
        ct.tag_configure("system_header", foreground=_C["fg_warning"], font=self._fonts["body_b"],
                         spacing1=8)
        ct.tag_configure("tool_header", foreground=_C["fg_tool_name"], font=self._fonts["mono_sm"],
                         spacing1=6)
        ct.tag_configure("error_text", foreground=_C["fg_error"])
        ct.tag_configure("dim_text", foreground=_C["fg_dim"])
        ct.tag_configure("bright_text", foreground=_C["fg_bright"])
        ct.tag_configure("accent_text", foreground=_C["fg_accent"])

        # Code block
        ct.tag_configure("code_block", background=_C["bg_code"], font=self._fonts["mono"],
                         foreground=_C["fg_code"], lmargin1=20, lmargin2=20, rmargin=20,
                         spacing1=4, spacing3=4)
        ct.tag_configure("code_lang", foreground=_C["fg_dim"], font=self._fonts["mono_sm"])

        # Syntax highlights
        ct.tag_configure("kw", foreground=_C["fg_keyword"], font=self._fonts["mono"])
        ct.tag_configure("str", foreground=_C["fg_string"], font=self._fonts["mono"])
        ct.tag_configure("num", foreground=_C["fg_number"], font=self._fonts["mono"])
        ct.tag_configure("cmt", foreground=_C["fg_comment"], font=self._fonts["mono"])

        # Tool call block
        ct.tag_configure("tool_block", background=_C["bg_tool"], font=self._fonts["mono_sm"],
                         foreground=_C["fg"], lmargin1=20, lmargin2=20, rmargin=20,
                         spacing1=2, spacing3=2)
        ct.tag_configure("tool_result", background=_C["bg_tool_result"], font=self._fonts["mono_sm"],
                         foreground=_C["fg_success"], lmargin1=20, lmargin2=20, rmargin=20)

        # Inline
        ct.tag_configure("inline_code", background="#2a2a3a", font=self._fonts["mono_sm"],
                         foreground=_C["fg_code"])
        ct.tag_configure("bold", font=self._fonts["body_b"])

        # Separator
        ct.tag_configure("separator", foreground=_C["border"], justify="center")

        # Stream cursor
        ct.tag_configure("stream_cursor", foreground=_C["fg_accent"])

    # ================================================================
    # Chat rendering
    # ================================================================

    def _append_text(self, text: str, *tags: str) -> None:
        ct = self._chat_text
        if not ct:
            return
        ct.configure(state="normal")
        ct.insert("end", text, tags)
        ct.configure(state="disabled")
        ct.see("end")

    def _append_system_message(self, text: str) -> None:
        self._append_text("● System\n", "system_header")
        self._append_text(text + "\n\n", "dim_text")

    def _render_user_message(self, content: str) -> None:
        self._append_text("● You\n", "user_header")
        self._append_text(content + "\n\n")

    def _render_assistant_header(self) -> None:
        self._append_text("● Assistant", "assistant_header")
        model = ""
        if self._engine:
            model = self._engine.config.effective_model
        if model:
            self._append_text(f"  ({model})", "dim_text")
        self._append_text("\n")

    def _render_assistant_content(self, content: str) -> None:
        self._render_markdown(content)
        self._append_text("\n")

    def _render_markdown(self, text: str) -> None:
        """Parse markdown and render with tags. Handles code blocks, inline code, bold."""
        parts = re.split(r'(```[\s\S]*?```)', text)
        for part in parts:
            if part.startswith("```") and part.endswith("```"):
                inner = part[3:-3]
                lang = ""
                if "\n" in inner:
                    first_line, rest = inner.split("\n", 1)
                    if first_line.strip().isalnum():
                        lang = first_line.strip()
                        inner = rest
                    else:
                        inner = first_line + "\n" + rest
                if lang:
                    self._append_text(f" {lang} \n", "code_lang")
                self._render_code_block(inner, lang)
                self._append_text("\n")
            else:
                self._render_inline_markdown(part)

    def _render_code_block(self, code: str, lang: str = "") -> None:
        ct = self._chat_text
        if not ct:
            return
        ct.configure(state="normal")

        # Copy button (embedded widget)
        copy_frame = tk.Frame(ct, bg=_C["bg_code"])
        copy_btn = tk.Label(
            copy_frame, text="Copy", font=self._fonts["small"],
            bg="#111122", fg=_C["fg_dim"], cursor="hand2", padx=6, pady=1,
        )
        copy_btn.pack(side="right", padx=4, pady=2)
        _code = code  # capture for closure
        copy_btn.bind("<Button-1>", lambda _, c=_code: self._copy_to_clipboard(c))
        copy_btn.bind("<Enter>", lambda e: e.widget.configure(fg=_C["fg"]))
        copy_btn.bind("<Leave>", lambda e: e.widget.configure(fg=_C["fg_dim"]))
        ct.window_create("end", window=copy_frame)
        ct.insert("end", "\n")

        # Syntax-highlighted code lines
        for line in code.split("\n"):
            self._render_highlighted_line(line, lang)

        ct.configure(state="disabled")
        ct.see("end")

    _KW_PYTHON = {
        "def", "class", "import", "from", "return", "if", "elif", "else",
        "for", "while", "try", "except", "finally", "with", "as", "yield",
        "async", "await", "lambda", "pass", "break", "continue", "raise",
        "True", "False", "None", "and", "or", "not", "in", "is",
    }
    _KW_JS = {
        "function", "const", "let", "var", "return", "if", "else", "for",
        "while", "try", "catch", "finally", "async", "await", "class",
        "import", "export", "from", "new", "this", "true", "false", "null",
        "undefined", "typeof", "instanceof", "switch", "case", "break",
        "default", "throw", "yield", "of", "in",
    }

    def _render_highlighted_line(self, line: str, lang: str = "") -> None:
        ct = self._chat_text
        if not ct:
            return
        keywords = self._KW_PYTHON if lang in ("python", "py") else self._KW_JS

        # Simple token-based highlighting
        i = 0
        while i < len(line):
            ch = line[i]
            # String literals
            if ch in ('"', "'"):
                end = line.find(ch, i + 1)
                if end == -1:
                    end = len(line) - 1
                ct.insert("end", line[i:end + 1], ("code_block", "str"))
                i = end + 1
            # Comments
            elif ch == '#' or (ch == '/' and i + 1 < len(line) and line[i + 1] == '/'):
                ct.insert("end", line[i:], ("code_block", "cmt"))
                i = len(line)
            # Numbers
            elif ch.isdigit():
                j = i
                while j < len(line) and (line[j].isdigit() or line[j] in '.xXabcdefABCDEF_'):
                    j += 1
                ct.insert("end", line[i:j], ("code_block", "num"))
                i = j
            # Words (keywords or identifiers)
            elif ch.isalpha() or ch == '_':
                j = i
                while j < len(line) and (line[j].isalnum() or line[j] == '_'):
                    j += 1
                word = line[i:j]
                if word in keywords:
                    ct.insert("end", word, ("code_block", "kw"))
                else:
                    ct.insert("end", word, "code_block")
                i = j
            else:
                ct.insert("end", ch, "code_block")
                i += 1
        ct.insert("end", "\n", "code_block")

    def _render_inline_markdown(self, text: str) -> None:
        parts = re.split(r'(`[^`]+`)', text)
        for part in parts:
            if part.startswith("`") and part.endswith("`"):
                self._append_text(part[1:-1], "inline_code")
            else:
                bold_parts = re.split(r'(\*\*[^*]+\*\*)', part)
                for bp in bold_parts:
                    if bp.startswith("**") and bp.endswith("**"):
                        self._append_text(bp[2:-2], "bold")
                    else:
                        self._append_text(bp)

    def _render_tool_call(self, name: str, arguments: str) -> None:
        ct = self._chat_text
        if not ct:
            return
        ct.configure(state="normal")

        try:
            args = json.loads(arguments)
            formatted = json.dumps(args, ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            formatted = arguments

        # Collapsible tool call frame
        frame = tk.Frame(ct, bg=_C["bg_tool"], bd=0, padx=8, pady=4)
        header = tk.Frame(frame, bg=_C["bg_tool"])
        header.pack(fill="x")

        arrow_var = tk.StringVar(value="▸")
        arrow = tk.Label(header, textvariable=arrow_var, bg=_C["bg_tool"],
                         fg=_C["fg_tool_name"], font=self._fonts["mono_sm"])
        arrow.pack(side="left")
        lbl = tk.Label(header, text=f"Tool Call: {name}", bg=_C["bg_tool"],
                       fg=_C["fg_tool_name"], font=self._fonts["mono_sm"], cursor="hand2")
        lbl.pack(side="left", padx=4)

        body = tk.Label(frame, text=formatted, bg=_C["bg_tool"],
                        fg=_C["fg_dim"], font=self._fonts["mono_sm"],
                        anchor="nw", justify="left", wraplength=600)

        def _toggle(event=None):
            if body.winfo_ismapped():
                body.pack_forget()
                arrow_var.set("▸")
            else:
                body.pack(fill="x", padx=(16, 4), pady=(2, 4))
                arrow_var.set("▾")
        lbl.bind("<Button-1>", _toggle)
        arrow.bind("<Button-1>", _toggle)
        header.bind("<Button-1>", _toggle)

        ct.window_create("end", window=frame)
        ct.insert("end", "\n")
        ct.configure(state="disabled")
        ct.see("end")

    def _render_tool_result(self, name: str, result: str) -> None:
        ct = self._chat_text
        if not ct:
            return
        ct.configure(state="normal")

        try:
            data = json.loads(result)
            formatted = json.dumps(data, ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            formatted = result

        # Collapsible result frame
        frame = tk.Frame(ct, bg=_C["bg_tool_result"], bd=0, padx=8, pady=4)
        header = tk.Frame(frame, bg=_C["bg_tool_result"])
        header.pack(fill="x")

        arrow_var = tk.StringVar(value="▸")
        arrow = tk.Label(header, textvariable=arrow_var, bg=_C["bg_tool_result"],
                         fg=_C["fg_success"], font=self._fonts["mono_sm"])
        arrow.pack(side="left")
        lbl = tk.Label(header, text=f"Result: {name}", bg=_C["bg_tool_result"],
                       fg=_C["fg_success"], font=self._fonts["mono_sm"], cursor="hand2")
        lbl.pack(side="left", padx=4)

        # Copy result button
        copy_btn = tk.Label(header, text="Copy", bg=_C["bg_tool_result"],
                            fg=_C["fg_dim"], font=self._fonts["small"],
                            cursor="hand2", padx=4)
        copy_btn.pack(side="right")
        copy_btn.bind("<Button-1>", lambda _, r=formatted: self._copy_to_clipboard(r))

        lines = formatted.split("\n")
        display = "\n".join(lines[:40])
        if len(lines) > 40:
            display += f"\n... ({len(lines) - 40} more lines)"
        body = tk.Label(frame, text=display, bg=_C["bg_tool_result"],
                        fg=_C["fg_dim"], font=self._fonts["mono_sm"],
                        anchor="nw", justify="left", wraplength=600)

        def _toggle(event=None):
            if body.winfo_ismapped():
                body.pack_forget()
                arrow_var.set("▸")
            else:
                body.pack(fill="x", padx=(16, 4), pady=(2, 4))
                arrow_var.set("▾")
        lbl.bind("<Button-1>", _toggle)
        arrow.bind("<Button-1>", _toggle)
        header.bind("<Button-1>", _toggle)

        ct.window_create("end", window=frame)
        ct.insert("end", "\n")
        ct.configure(state="disabled")
        ct.see("end")

    def _copy_to_clipboard(self, text: str) -> None:
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._set_status("Copied to clipboard")
        except Exception:
            pass

    # ================================================================
    # Chat controller callbacks (called from background thread)
    # ================================================================

    def _on_message_added(self, msg) -> None:
        from ai_editor.chat_state import ChatMessage
        def _update():
            if msg.role == "user":
                self._render_user_message(msg.content)
                self._set_status("Thinking...")
            elif msg.role == "assistant" and msg.is_streaming:
                self._render_assistant_header()
                ct = self._chat_text
                if ct:
                    ct.configure(state="normal")
                    # Mark the stream start so we can replace raw text later
                    ct.mark_set("stream_start", "end-1c")
                    ct.mark_gravity("stream_start", "left")
                    ct.configure(state="disabled")
            elif msg.role == "tool":
                self._render_tool_result(msg.tool_name or "?", msg.content)
        if self._win and self._win.winfo_exists():
            self._win.after(0, _update)

    def _on_stream_delta(self, msg, text: str) -> None:
        def _update():
            ct = self._chat_text
            if not ct:
                return
            ct.configure(state="normal")
            ct.insert("end", text)
            ct.configure(state="disabled")
            ct.see("end")
        if self._win and self._win.winfo_exists():
            self._win.after(0, _update)

    def _on_stream_end(self, msg) -> None:
        def _update():
            ct = self._chat_text
            if not ct:
                return

            # Re-render: delete raw streamed text, insert markdown-formatted version
            if msg.content and not msg.is_error:
                try:
                    ct.configure(state="normal")
                    ct.delete("stream_start", "end-1c")
                    ct.configure(state="disabled")
                    self._render_markdown(msg.content)
                except tk.TclError:
                    pass

            if msg.tool_calls:
                for tc in msg.tool_calls:
                    self._render_tool_call(tc.name, tc.arguments)
            if msg.is_error:
                self._append_text(f"\n⚠ Error: {msg.content}\n", "error_text")
            self._append_text("\n")

            tokens = msg.usage.get("total_tokens", 0)
            if tokens:
                self._token_lbl.configure(
                    text=f"{self._controller.conversation.total_tokens} tokens"
                )
            self._set_status("Ready" if not msg.tool_calls else "Executing tools...")
        if self._win and self._win.winfo_exists():
            self._win.after(0, _update)

    def _on_tool_start(self, call_id: str, name: str, args: str) -> None:
        def _update():
            self._set_status(f"Tool: {name}...")
        if self._win and self._win.winfo_exists():
            self._win.after(0, _update)

    def _on_tool_end(self, call_id: str, result: str) -> None:
        pass  # Rendering handled by on_message_added for tool messages

    def _on_error(self, error: str) -> None:
        def _update():
            self._append_text(f"\n⚠ Error: {error}\n\n", "error_text")
            self._set_status("Error")
        if self._win and self._win.winfo_exists():
            self._win.after(0, _update)

    def _on_idle(self) -> None:
        def _update():
            self._set_status("Ready")
            self._cancel_btn.configure(fg=_C["fg_dim"])
        if self._win and self._win.winfo_exists():
            self._win.after(0, _update)

    # ================================================================
    # User actions
    # ================================================================

    def _on_enter(self, event) -> str:
        if event.state & 0x1:  # Shift held
            return ""  # Let default handler insert newline
        self._do_send()
        return "break"

    def _do_send(self) -> None:
        if not self._input_text:
            return
        text = self._input_text.get("1.0", "end-1c").strip()
        if not text:
            return

        self._hide_slash_popup()

        # Save to input history
        if not self._input_history or self._input_history[-1] != text:
            self._input_history.append(text)
            if len(self._input_history) > 100:
                self._input_history = self._input_history[-100:]
        self._history_idx = -1

        self._input_text.delete("1.0", "end")

        # Intercept slash commands
        if text.startswith("/"):
            self._execute_slash_command(text)
            return

        self._ensure_engine()

        # Apply current UI config
        self._sync_ui_to_config()

        if not self._engine.config.api_key and self._engine.config.provider != "ollama":
            self._append_text("⚠ 请先在 ⚙ Settings 中配置 API Key\n\n", "error_text")
            return

        self._cancel_btn.configure(fg=_C["fg_error"])
        self._controller.send(text)

    def _on_history_up(self, event) -> str:
        if not self._input_history:
            return ""
        # Only navigate history when cursor is on the first line
        if self._input_text and int(self._input_text.index("insert").split(".")[0]) > 1:
            return ""
        if self._history_idx == -1:
            self._history_idx = len(self._input_history) - 1
        elif self._history_idx > 0:
            self._history_idx -= 1
        else:
            return "break"
        self._input_text.delete("1.0", "end")
        self._input_text.insert("1.0", self._input_history[self._history_idx])
        return "break"

    def _on_history_down(self, event) -> str:
        if not self._input_history or self._history_idx == -1:
            return ""
        if self._history_idx < len(self._input_history) - 1:
            self._history_idx += 1
            self._input_text.delete("1.0", "end")
            self._input_text.insert("1.0", self._input_history[self._history_idx])
        else:
            self._history_idx = -1
            self._input_text.delete("1.0", "end")
        return "break"

    def _do_cancel(self) -> None:
        if self._controller and self._controller.is_running:
            self._controller.cancel()
            self._set_status("Cancelled")

    def _new_chat(self) -> None:
        if self._controller:
            self._controller.cancel()
            sp = self._engine.config.system_prompt or self._default_system_prompt()
            self._controller.new_conversation(sp)
        ct = self._chat_text
        if ct:
            ct.configure(state="normal")
            ct.delete("1.0", "end")
            ct.configure(state="disabled")
        self._append_system_message("New conversation started.")
        self._token_lbl.configure(text="0 tokens")
        self._set_status("Ready")

    def _clear_chat(self) -> None:
        self._new_chat()

    def _export_chat(self) -> None:
        if not self._controller:
            return
        export_data = self._controller.export_messages()
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(export_data)
            self._set_status("Exported to clipboard")
        except Exception:
            self._set_status("Export failed")

    def _set_status(self, text: str) -> None:
        if self._status_var:
            self._status_var.set(text)

    # ================================================================
    # Config sync
    # ================================================================

    def _sync_config_to_ui(self) -> None:
        config = self._load_config()
        if self._provider_var:
            self._provider_var.set(config.provider)
        if self._model_var:
            self._model_var.set(config.effective_model)

    def _sync_ui_to_config(self) -> None:
        if not self._engine:
            return
        if self._provider_var:
            self._engine.config.provider = self._provider_var.get()
        if self._model_var:
            model = self._model_var.get().strip()
            if model:
                self._engine.config.model = model

    def _on_provider_change(self) -> None:
        provider = self._provider_var.get() if self._provider_var else "openai"
        from ai_editor.llm_engine import _PROVIDER_DEFAULTS
        defaults = _PROVIDER_DEFAULTS.get(provider, {})
        if self._model_var and not self._model_var.get().strip():
            self._model_var.set(defaults.get("default_model", ""))

    # ================================================================
    # Settings dialog
    # ================================================================

    def _show_settings_dialog(self) -> None:
        self._ensure_engine()
        dlg = tk.Toplevel(self._win or self.root)
        dlg.title("AI Editor Settings")
        dlg.geometry("520x580")
        dlg.configure(bg=_C["bg"])
        dlg.transient(self._win)
        dlg.grab_set()
        dlg.attributes("-topmost", True)

        config = self._engine.config

        body = tk.Frame(dlg, bg=_C["bg"], padx=20, pady=16)
        body.pack(fill="both", expand=True)

        row = 0

        def _label(text: str, r: int) -> None:
            tk.Label(body, text=text, bg=_C["bg"], fg=_C["fg"],
                     font=self._fonts["body"], anchor="w").grid(
                row=r, column=0, sticky="w", pady=(8, 2))

        def _entry(var: tk.StringVar, r: int, show: str = "") -> tk.Entry:
            e = tk.Entry(body, textvariable=var, bg=_C["bg_input"], fg=_C["fg"],
                         insertbackground=_C["fg"], relief="flat", font=self._fonts["mono"],
                         show=show)
            e.grid(row=r, column=0, sticky="ew", pady=2, columnspan=2)
            return e

        _label("Provider", row); row += 1
        prov_var = tk.StringVar(value=config.provider)
        prov_cb = ttk.Combobox(body, textvariable=prov_var, state="readonly", width=20,
                               values=["openai", "anthropic", "deepseek", "ollama", "custom"])
        prov_cb.grid(row=row, column=0, sticky="w", pady=2, columnspan=2); row += 1

        _label("API Key", row); row += 1
        key_var = tk.StringVar(value=config.api_key)
        key_entry = _entry(key_var, row, show="•"); row += 1

        # Toggle visibility
        show_key = tk.BooleanVar(value=False)
        def _toggle_key():
            key_entry.configure(show="" if show_key.get() else "•")
        tk.Checkbutton(body, text="Show key", variable=show_key, command=_toggle_key,
                       bg=_C["bg"], fg=_C["fg_dim"], selectcolor=_C["bg_input"],
                       activebackground=_C["bg"]).grid(row=row, column=0, sticky="w"); row += 1

        _label("Base URL (leave empty for default)", row); row += 1
        url_var = tk.StringVar(value=config.base_url)
        _entry(url_var, row); row += 1

        _label("Model", row); row += 1
        model_var = tk.StringVar(value=config.model)
        _entry(model_var, row); row += 1

        _label("Temperature", row); row += 1
        temp_var = tk.StringVar(value=str(config.temperature))
        _entry(temp_var, row); row += 1

        _label("Max Tokens", row); row += 1
        maxtok_var = tk.StringVar(value=str(config.max_tokens))
        _entry(maxtok_var, row); row += 1

        _label("System Prompt", row); row += 1
        sp_text = tk.Text(body, bg=_C["bg_input"], fg=_C["fg"], font=self._fonts["mono_sm"],
                          height=6, wrap="word", insertbackground=_C["fg"], relief="flat")
        sp_text.grid(row=row, column=0, sticky="nsew", pady=2, columnspan=2); row += 1
        sp_text.insert("1.0", config.system_prompt)

        body.grid_columnconfigure(0, weight=1)

        # Buttons
        btn_frame = tk.Frame(body, bg=_C["bg"])
        btn_frame.grid(row=row, column=0, columnspan=2, pady=(12, 0), sticky="ew")

        def _test():
            from ai_editor.llm_engine import ProviderConfig
            test_cfg = ProviderConfig(
                provider=prov_var.get(), api_key=key_var.get(),
                base_url=url_var.get(), model=model_var.get(),
            )
            ok, msg = self._engine.test_connection(test_cfg)
            _status_lbl.configure(text=msg[:80], fg=_C["fg_success"] if ok else _C["fg_error"])

        def _save():
            from ai_editor.llm_engine import ProviderConfig
            new_cfg = ProviderConfig(
                provider=prov_var.get(),
                api_key=key_var.get(),
                base_url=url_var.get(),
                model=model_var.get(),
                temperature=float(temp_var.get() or 0.7),
                max_tokens=int(maxtok_var.get() or 4096),
                system_prompt=sp_text.get("1.0", "end-1c").strip(),
            )
            self._engine.config = new_cfg
            self._save_config(new_cfg)
            self._sync_config_to_ui()
            # Rebuild system prompt in conversation
            if self._controller and self._controller.conversation:
                self._controller.conversation.system_prompt = (
                    new_cfg.system_prompt or self._default_system_prompt()
                )
            dlg.destroy()

        test_btn = tk.Label(
            btn_frame, text="  Test Connection  ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_accent"], cursor="hand2", padx=6, pady=2,
        )
        test_btn.pack(side="left")
        test_btn.bind("<Button-1>", lambda _: _test())

        save_btn = tk.Label(
            btn_frame, text="  Save  ", font=self._fonts["body_b"],
            bg=_C["button"], fg="#ffffff", cursor="hand2", padx=12, pady=2,
        )
        save_btn.pack(side="right")
        save_btn.bind("<Button-1>", lambda _: _save())

        cancel_dlg_btn = tk.Label(
            btn_frame, text="  Cancel  ", font=self._fonts["body"],
            bg=_C["bg_secondary"], fg=_C["fg_dim"], cursor="hand2", padx=8, pady=2,
        )
        cancel_dlg_btn.pack(side="right", padx=6)
        cancel_dlg_btn.bind("<Button-1>", lambda _: dlg.destroy())

        _status_lbl = tk.Label(body, text="", bg=_C["bg"], fg=_C["fg_dim"],
                               font=self._fonts["small"], anchor="w")
        _status_lbl.grid(row=row + 1, column=0, columnspan=2, sticky="w", pady=(4, 0))

    # ================================================================
    # History sidebar
    # ================================================================

    def _toggle_sidebar(self) -> None:
        if self._sidebar_visible:
            self._sidebar.pack_forget()
            self._sidebar_visible = False
        else:
            self._sidebar.pack(side="left", fill="y", before=self._chat_frame)
            self._sidebar_visible = True
            self._refresh_sidebar_history()

    def _refresh_sidebar_history(self) -> None:
        for w in self._sidebar_list_frame.winfo_children():
            w.destroy()
        try:
            from ai_editor.history import list_conversations
            entries = list_conversations(limit=30)
        except Exception:
            entries = []

        if not entries:
            tk.Label(
                self._sidebar_list_frame, text="No history yet",
                bg=_C["bg_sidebar"], fg=_C["fg_dim"], font=self._fonts["small"],
                padx=12, pady=8,
            ).pack(fill="x")
            return

        for entry in entries:
            item = tk.Frame(self._sidebar_list_frame, bg=_C["bg_sidebar"], padx=8, pady=3)
            item.pack(fill="x")

            title = entry.get("title", "Untitled")[:30]
            count = entry.get("message_count", 0)
            model = entry.get("model", "")

            lbl = tk.Label(
                item, text=title, bg=_C["bg_sidebar"], fg=_C["fg"],
                font=self._fonts["small"], anchor="w", cursor="hand2",
            )
            lbl.pack(fill="x")
            meta = tk.Label(
                item, text=f"{count} msgs · {model[:15]}" if model else f"{count} msgs",
                bg=_C["bg_sidebar"], fg=_C["fg_dim"],
                font=self._fonts["small"], anchor="w",
            )
            meta.pack(fill="x")

            cid = entry["id"]
            lbl.bind("<Button-1>", lambda _, i=cid: self._load_history_conversation(i))
            item.bind("<Enter>", lambda e, w=item: w.configure(bg="#2a2a2a"))
            item.bind("<Leave>", lambda e, w=item: w.configure(bg=_C["bg_sidebar"]))

    def _load_history_conversation(self, conv_id: str) -> None:
        try:
            from ai_editor.history import load_conversation
            from ai_editor.chat_state import ChatMessage, Conversation
            from ai_editor.llm_engine import ToolCall
        except ImportError:
            return

        data = load_conversation(conv_id)
        if not data:
            self._set_status("Failed to load conversation")
            return

        self._ensure_engine()

        conv = Conversation(system_prompt=data.get("system_prompt", ""))
        conv.id = data.get("id", conv_id)
        conv.title = data.get("title", "Loaded")

        for m in data.get("messages", []):
            msg = ChatMessage(
                role=m.get("role", "user"),
                content=m.get("content", ""),
                timestamp=m.get("timestamp", 0),
            )
            if m.get("tool_call_id"):
                msg.tool_call_id = m["tool_call_id"]
                msg.tool_name = m.get("tool_name")
            if m.get("tool_calls"):
                for tc in m["tool_calls"]:
                    msg.tool_calls.append(ToolCall(
                        id=tc.get("id", ""),
                        name=tc.get("name", ""),
                        arguments=tc.get("arguments", ""),
                        result=tc.get("result"),
                    ))
            conv.messages.append(msg)

        self._controller.cancel()
        self._controller.conversation = conv

        # Re-render all messages
        ct = self._chat_text
        if ct:
            ct.configure(state="normal")
            ct.delete("1.0", "end")
            ct.configure(state="disabled")

        for msg in conv.messages:
            if msg.role == "user":
                self._render_user_message(msg.content)
            elif msg.role == "assistant":
                self._render_assistant_header()
                if msg.content:
                    self._render_markdown(msg.content)
                    self._append_text("\n")
                for tc in msg.tool_calls:
                    self._render_tool_call(tc.name, tc.arguments)
            elif msg.role == "tool":
                self._render_tool_result(msg.tool_name or "?", msg.content)

        self._set_status(f"Loaded: {conv.title}")
        self._token_lbl.configure(text=f"{conv.total_tokens} tokens")

    # ================================================================
    # Tools panel
    # ================================================================

    def _show_tools_panel(self) -> None:
        self._ensure_engine()
        dlg = tk.Toplevel(self._win or self.root)
        dlg.title("Available Tools")
        dlg.geometry("600x500")
        dlg.configure(bg=_C["bg"])
        dlg.transient(self._win)
        dlg.attributes("-topmost", True)

        header = tk.Label(
            dlg, text="🔧 Engine Tools", font=self._fonts["heading"],
            bg=_C["bg_secondary"], fg=_C["fg_accent"], padx=12, pady=8, anchor="w",
        )
        header.pack(fill="x")

        text = tk.Text(
            dlg, bg=_C["bg"], fg=_C["fg"], font=self._fonts["mono_sm"],
            wrap="word", state="disabled", relief="flat", padx=12, pady=8,
        )
        scroll = tk.Scrollbar(dlg, command=text.yview, bg=_C["scrollbar"],
                              troughcolor=_C["bg"], width=8)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        text.pack(fill="both", expand=True)

        text.tag_configure("cat", foreground=_C["fg_accent"], font=self._fonts["body_b"])
        text.tag_configure("name", foreground=_C["fg_tool_name"], font=self._fonts["mono"])
        text.tag_configure("desc", foreground=_C["fg_dim"])
        text.tag_configure("confirm", foreground=_C["fg_warning"])

        text.configure(state="normal")
        for cat in self._registry.categories():
            text.insert("end", f"\n▸ {cat}\n", "cat")
            for tool in self._registry.list_tools(cat):
                text.insert("end", f"  {tool.name}", "name")
                if tool.requires_confirm:
                    text.insert("end", " ⚠", "confirm")
                text.insert("end", f"\n    {tool.description}\n", "desc")
        text.configure(state="disabled")

    # ================================================================
    # Slash commands (VSCode-style / prefix)
    # ================================================================

    _SLASH_COMMANDS = [
        ("/clear",    "清空当前对话"),
        ("/new",      "新建对话"),
        ("/export",   "导出对话到剪贴板"),
        ("/history",  "显示/隐藏历史侧边栏"),
        ("/tools",    "查看可用工具列表"),
        ("/settings", "打开设置对话框"),
        ("/system",   "查看系统信息"),
        ("/eval",     "执行Python表达式 (用法: /eval <expr>)"),
    ]

    def _on_input_key_release(self, event) -> None:
        if not self._input_text:
            return
        text = self._input_text.get("1.0", "end-1c")
        if text.startswith("/") and "\n" not in text:
            self._show_slash_popup(text)
        else:
            self._hide_slash_popup()

    def _show_slash_popup(self, prefix: str) -> None:
        matches = [
            (cmd, desc) for cmd, desc in self._SLASH_COMMANDS
            if cmd.startswith(prefix.split()[0].lower())
        ]
        if not matches:
            self._hide_slash_popup()
            return

        if not self._slash_popup or not self._slash_popup.winfo_exists():
            self._slash_popup = tk.Toplevel(self._win or self.root)
            self._slash_popup.overrideredirect(True)
            self._slash_popup.attributes("-topmost", True)
            self._slash_popup.configure(bg=_C["bg_secondary"])

        popup = self._slash_popup

        # Position above input
        try:
            x = self._input_text.winfo_rootx() + 12
            y = self._input_text.winfo_rooty() - min(len(matches), 8) * 24 - 4
            popup.geometry(f"+{x}+{y}")
        except Exception:
            pass

        for w in popup.winfo_children():
            w.destroy()

        for cmd, desc in matches[:8]:
            row = tk.Frame(popup, bg=_C["bg_secondary"], padx=8, pady=2)
            row.pack(fill="x")
            tk.Label(row, text=cmd, bg=_C["bg_secondary"], fg=_C["fg_accent"],
                     font=self._fonts["mono_sm"], anchor="w", width=12).pack(side="left")
            tk.Label(row, text=desc, bg=_C["bg_secondary"], fg=_C["fg_dim"],
                     font=self._fonts["small"], anchor="w").pack(side="left", padx=4)
            row.bind("<Button-1>", lambda _, c=cmd: self._select_slash_command(c))
            row.bind("<Enter>", lambda e, r=row: r.configure(bg="#333333"))
            row.bind("<Leave>", lambda e, r=row: r.configure(bg=_C["bg_secondary"]))

        popup.deiconify()
        popup.lift()

    def _hide_slash_popup(self) -> None:
        if self._slash_popup and self._slash_popup.winfo_exists():
            self._slash_popup.withdraw()

    def _select_slash_command(self, cmd: str) -> None:
        self._hide_slash_popup()
        if not self._input_text:
            return
        self._input_text.delete("1.0", "end")
        self._execute_slash_command(cmd)

    def _execute_slash_command(self, raw: str) -> None:
        parts = raw.strip().split(None, 1)
        cmd = parts[0].lower()
        arg = parts[1] if len(parts) > 1 else ""

        if cmd == "/clear":
            self._clear_chat()
        elif cmd == "/new":
            self._new_chat()
        elif cmd == "/export":
            self._export_chat()
        elif cmd == "/history":
            self._toggle_sidebar()
        elif cmd == "/tools":
            self._show_tools_panel()
        elif cmd == "/settings":
            self._show_settings_dialog()
        elif cmd == "/system":
            self._run_slash_tool(cmd)
        elif cmd == "/eval" and arg:
            self._run_slash_eval(arg)
        else:
            self._append_text(f"Unknown command: {cmd}\n", "dim_text")

    def _run_slash_tool(self, cmd: str) -> None:
        self._ensure_engine()
        tool_map = {
            "/system": {"action": "system_info"},
        }
        payload = tool_map.get(cmd)
        if not payload:
            return
        result = self._registry.execute("engine", json.dumps(payload, ensure_ascii=False))
        self._append_text(f"● {cmd}\n", "system_header")
        try:
            data = json.loads(result)
            formatted = json.dumps(data, ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            formatted = result
        self._append_text(formatted + "\n\n", "dim_text")

    def _run_slash_eval(self, expr: str) -> None:
        self._ensure_engine()
        result = self._registry.execute(
            "engine",
            json.dumps({"action": "eval", "expression": expr}, ensure_ascii=False),
        )
        self._append_text(f"● /eval {expr}\n", "system_header")
        try:
            data = json.loads(result)
            formatted = json.dumps(data, ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            formatted = result
        self._append_text(formatted + "\n\n", "dim_text")
