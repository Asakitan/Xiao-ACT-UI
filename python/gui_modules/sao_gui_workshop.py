# -*- coding: utf-8 -*-
"""Creative Workshop — SAO menu sidebar category + floating panel.

Two widgets:
  * ``WorkshopChildPreview``: compact preview inside the SAO menu childbar area
    (search + featured list + "open full panel" button).
  * ``WorkshopPanel``: full-featured floating Tk Toplevel with tabs:
    - 在线商店 (online catalog, install from workshop)
    - 我的插件 (local user plugins, delete/publish/open in AI editor)
    - 上传 (publish a local plugin to workshop)

Both use ``workshop.client.WorkshopClient`` for catalog data (HTTP, threaded).
"""

from __future__ import annotations

import json
import math
import os
import threading
import tkinter as tk
from tkinter import filedialog
from typing import Any, Dict, List, Mapping, Optional

from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules.sao_panel_ui import (
    _apply_window_icon,
    _bind_panel_drag,
)

# ── White + Gold palette ────────────────────────────────────────────
_WG_BG         = '#FAF8F2'
_WG_HEADER_BG  = '#F5F0E4'
_WG_HEADER_FG  = '#8B7D5A'
_WG_BORDER     = '#E2D5B0'
_WG_GOLD       = '#D4A520'
_WG_GOLD_DARK  = '#B8941C'
_WG_GOLD_SOFT  = '#FFF6E0'
_WG_BODY_BG    = '#FFFCF5'
_WG_CARD_BG    = '#FFFFFF'
_WG_CARD_HOVER = '#FFF8E0'
_WG_TEXT       = '#3D3929'
_WG_MUTED      = '#9A9488'
_WG_SEP        = '#EBE0C8'
_WG_GREEN      = '#5EAA6C'
_WG_RED        = '#D04040'
_WG_BADGE_BG   = '#F0E8D0'

_CARD_W = 260
_CARD_PAD = 8
_COLS = 3


def _finite_int(v: Any, default: int = 0, *, lo: int | None = None,
                hi: int | None = None) -> int:
    try:
        n = float(default if v is None or v == '' else v)
    except Exception:
        n = float(default or 0)
    if not math.isfinite(n):
        n = float(default or 0)
    if lo is not None:
        n = max(float(lo), n)
    if hi is not None:
        n = min(float(hi), n)
    return int(n)


def _is_paid() -> bool:
    try:
        from workshop.app import _is_paid_user
        return _is_paid_user()
    except Exception:
        return False


def _get_workshop_client():
    try:
        from workshop.client import WorkshopClient
        from workshop.app import _get_server_url, _get_api_key, _is_paid_user, get_workshop_token
        return WorkshopClient(
            base_url=_get_server_url(),
            api_key=_get_api_key(),
            is_paid=_is_paid_user(),
            workshop_token=get_workshop_token(),
        )
    except Exception:
        return None


def _open_ai_editor_via_owner(owner: Any, *, status_var: Any = None) -> None:
    """Open AI Editor through the owner path that releases menu/fisheye input first."""
    try:
        safe_toggle = getattr(owner, "_toggle_ai_editor_panel", None)
        if callable(safe_toggle):
            root = getattr(owner, "root", None)
            if root is not None and hasattr(root, "after_idle"):
                root.after_idle(safe_toggle)
            else:
                safe_toggle()
            return
        from ai_editor.app import launch
        launch(gui_ref=owner)
    except Exception as exc:
        if status_var is not None:
            try:
                status_var.set(f"AI Editor: {exc}")
            except Exception:
                pass
        else:
            raise


def _get_installed_ids(owner: Any) -> list[str]:
    try:
        from act_platform.runtime import act_plugin_status
        status = act_plugin_status(owner)
        return [str(p.get('id') or '') for p in (status.get('plugins') or [])]
    except Exception:
        pass
    try:
        from config import BASE_DIR
        ids = []
        for base in (os.path.join(BASE_DIR, 'plugins'), os.path.join(BASE_DIR, 'user_plugins')):
            if not os.path.isdir(base):
                continue
            for name in os.listdir(base):
                if os.path.isfile(os.path.join(base, name, 'plugin.json')):
                    ids.append(name)
        return ids
    except Exception:
        return []


def _get_local_user_plugins() -> list[dict]:
    """List plugins from user_plugins/ (non-builtin, user-installed)."""
    results = []
    try:
        from config import BASE_DIR
        user_dir = os.path.join(BASE_DIR, 'user_plugins')
        if not os.path.isdir(user_dir):
            return results
        for name in sorted(os.listdir(user_dir)):
            plugin_json = os.path.join(user_dir, name, 'plugin.json')
            if not os.path.isfile(plugin_json):
                continue
            try:
                with open(plugin_json, 'r', encoding='utf-8') as f:
                    meta = json.load(f)
            except Exception:
                meta = {}
            results.append({
                'id': name,
                'name': str(meta.get('name') or name),
                'version': str(meta.get('version') or '0.0.0'),
                'author': str(meta.get('author') or ''),
                'description': str(meta.get('description') or ''),
                'path': os.path.join(user_dir, name),
                'source': 'user',
            })
    except Exception:
        pass
    return results


def _make_tab_btn(parent, text, command, bg=_WG_CARD_BG, fg=_WG_TEXT,
                  active_bg=_WG_GOLD, active_fg='#FFFFFF'):
    btn = tk.Label(parent, text=text, bg=bg, fg=fg,
                   font=get_cjk_font(10, True), padx=12, pady=4, cursor='hand2')
    btn.pack(side='left', padx=(0, 4))
    btn.bind('<Button-1>', lambda e: command())
    btn._active_bg = active_bg
    btn._active_fg = active_fg
    btn._normal_bg = bg
    btn._normal_fg = fg
    return btn


def _set_tab_active(btns: dict, active_key: str):
    for key, btn in btns.items():
        if key == active_key:
            btn.configure(bg=btn._active_bg, fg=btn._active_fg)
        else:
            btn.configure(bg=btn._normal_bg, fg=btn._normal_fg)


# ═══════════════════════════════════════════════════════════════════
#  WorkshopChildPreview — inline in SAO menu (replaces childbar)
# ═══════════════════════════════════════════════════════════════════

class WorkshopChildPreview(tk.Frame):
    """Compact workshop preview embedded in the SAO menu childbar area."""

    _PREVIEW_W = 320
    _MAX_ROWS = 6

    def __init__(self, parent: tk.Widget, owner: Any,
                 on_open_full: Any = None):
        super().__init__(parent, bg=_WG_BODY_BG, highlightthickness=0)
        self._owner = owner
        self._on_open_full = on_open_full
        self._client = None
        self._plugins: list[dict] = []
        self._local_plugins: list[dict] = []
        self._installed: list[str] = []
        self._loading = False
        self._loaded = False
        self._search_var = tk.StringVar()
        self._build()

    def _build(self):
        bg = _WG_BODY_BG
        self.configure(bg=bg, width=self._PREVIEW_W)

        # ── Header ──
        title_f = tk.Frame(self, bg=bg)
        title_f.pack(fill='x', padx=12, pady=(10, 0))
        hdr_row = tk.Frame(title_f, bg=bg)
        hdr_row.pack(fill='x')
        tk.Label(hdr_row, text='◇', bg=bg, fg=_WG_GOLD,
                 font=get_sao_font(12, True)).pack(side='left', padx=(0, 6))
        tk.Label(hdr_row, text='WORKSHOP', bg=bg, fg=_WG_GOLD,
                 font=get_sao_font(9, True), anchor='w').pack(side='left')
        tk.Label(title_f, text='创意工坊', bg=bg, fg=_WG_MUTED,
                 font=get_cjk_font(9), anchor='w').pack(fill='x', pady=(1, 0))

        tk.Frame(self, bg=_WG_GOLD, height=1).pack(fill='x', padx=12, pady=(6, 0))
        tk.Frame(self, bg=_WG_SEP, height=1).pack(fill='x', padx=12, pady=(1, 6))

        # ── Local plugins section ──
        local_hdr = tk.Frame(self, bg=bg)
        local_hdr.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(local_hdr, text='我的插件', bg=bg, fg=_WG_TEXT,
                 font=get_cjk_font(9, True), anchor='w').pack(side='left')

        self._local_frame = tk.Frame(self, bg=bg)
        self._local_frame.pack(fill='x', padx=12, pady=(0, 4))

        tk.Frame(self, bg=_WG_SEP, height=1).pack(fill='x', padx=12, pady=(2, 4))

        # ── Online section ──
        online_hdr = tk.Frame(self, bg=bg)
        online_hdr.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(online_hdr, text='在线商店', bg=bg, fg=_WG_TEXT,
                 font=get_cjk_font(9, True), anchor='w').pack(side='left')

        self._list_frame = tk.Frame(self, bg=bg)
        self._list_frame.pack(fill='both', expand=True, padx=12, pady=(0, 4))

        # ── Bottom ──
        tk.Frame(self, bg=_WG_SEP, height=1).pack(fill='x', padx=12, pady=(2, 0))
        bottom = tk.Frame(self, bg=bg)
        bottom.pack(fill='x', padx=12, pady=(6, 10))

        open_btn = tk.Label(bottom, text='全部浏览  →', bg=_WG_GOLD,
                            fg='#FFFFFF', font=get_cjk_font(10, True),
                            cursor='hand2', padx=12, pady=5)
        open_btn.pack(fill='x')
        open_btn.bind('<Button-1>', lambda e: self._open_full())
        open_btn.bind('<Enter>', lambda e: open_btn.configure(bg=_WG_GOLD_DARK))
        open_btn.bind('<Leave>', lambda e: open_btn.configure(bg=_WG_GOLD))

    def activate(self):
        self._local_plugins = _get_local_user_plugins()
        self._render_local()
        if not self._loaded:
            self._load_featured()

    def _render_local(self):
        for w in self._local_frame.winfo_children():
            w.destroy()
        plugins = self._local_plugins[:4]
        if not plugins:
            tk.Label(self._local_frame, text='无用户插件', bg=_WG_BODY_BG,
                     fg=_WG_MUTED, font=get_cjk_font(8)).pack(pady=2)
            return
        for p in plugins:
            self._render_local_row(p)

    def _render_local_row(self, plugin: dict):
        bg = _WG_CARD_BG
        pid = str(plugin.get('id') or '')
        name = str(plugin.get('name') or pid)

        row = tk.Frame(self._local_frame, bg=bg, highlightthickness=1,
                       highlightbackground=_WG_BORDER)
        row.pack(fill='x', pady=2)

        rail = tk.Frame(row, bg=_WG_GREEN, width=3)
        rail.pack(side='left', fill='y')

        inner = tk.Frame(row, bg=bg)
        inner.pack(fill='x', padx=6, pady=4)

        tk.Label(inner, text=name, bg=bg, fg=_WG_TEXT,
                 font=get_cjk_font(9, True), anchor='w').pack(side='left', fill='x', expand=True)

        # AI editor button
        ed_btn = tk.Label(inner, text='✦', bg=_WG_BADGE_BG, fg=_WG_GOLD,
                          font=get_cjk_font(9), padx=3, cursor='hand2')
        ed_btn.pack(side='right', padx=(2, 0))
        ed_btn.bind('<Button-1>', lambda e, p=pid: self._open_in_editor(p))

        row.bind('<Enter>', lambda e, r=row: r.configure(highlightbackground=_WG_GOLD))
        row.bind('<Leave>', lambda e, r=row: r.configure(highlightbackground=_WG_BORDER))

    def _open_in_editor(self, plugin_id: str):
        try:
            plugin_path = None
            for p in self._local_plugins:
                if p.get('id') == plugin_id:
                    plugin_path = p.get('path')
                    break
            if plugin_path:
                _open_ai_editor_via_owner(self._owner)
        except Exception:
            pass

    def _load_featured(self):
        if self._loading:
            return
        self._loading = True
        self._render_loading()
        self._installed = _get_installed_ids(self._owner)

        def _fetch():
            client = _get_workshop_client()
            if client is None:
                self.after(0, lambda: self._render_error('Workshop server unavailable'))
                self._loading = False
                return
            try:
                result = client.catalog(per_page=8)
                plugins = result.get('plugins') or [] if isinstance(result, dict) else []
                self.after(0, lambda p=plugins: self._on_loaded(p))
            except Exception as exc:
                self.after(0, lambda e=str(exc): self._render_error(e))
            self._loading = False

        threading.Thread(target=_fetch, daemon=True).start()

    def _on_loaded(self, plugins: list):
        self._plugins = list(plugins)
        self._loaded = True
        self._filter_render()

    def _filter_render(self):
        query = self._search_var.get().strip().lower()
        plugins = self._plugins
        if query:
            plugins = [p for p in plugins
                       if query in str(p.get('name') or '').lower()
                       or query in str(p.get('plugin_id') or '').lower()]
        self._render_list(plugins[:4])

    def _render_loading(self):
        for w in self._list_frame.winfo_children():
            w.destroy()
        tk.Label(self._list_frame, text='加载中...', bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(9)).pack(pady=6)

    def _render_error(self, msg: str):
        self._loading = False
        for w in self._list_frame.winfo_children():
            w.destroy()
        tk.Label(self._list_frame, text='离线', bg=_WG_BODY_BG,
                 fg=_WG_RED, font=get_cjk_font(9)).pack(pady=2)
        tk.Label(self._list_frame, text=str(msg)[:40], bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(8), wraplength=280).pack(pady=2)

    def _render_list(self, plugins: list):
        for w in self._list_frame.winfo_children():
            w.destroy()
        if not plugins:
            tk.Label(self._list_frame, text='无在线插件', bg=_WG_BODY_BG,
                     fg=_WG_MUTED, font=get_cjk_font(9)).pack(pady=6)
            return
        for p in plugins:
            self._render_row(p)

    def _render_row(self, plugin: dict):
        bg = _WG_CARD_BG
        pid = str(plugin.get('plugin_id') or '')
        name = str(plugin.get('name') or pid)
        installed = pid in self._installed

        row = tk.Frame(self._list_frame, bg=bg, highlightthickness=1,
                       highlightbackground=_WG_BORDER, cursor='hand2')
        row.pack(fill='x', pady=2)

        rail = tk.Frame(row, bg=_WG_GOLD if installed else _WG_SEP, width=3)
        rail.pack(side='left', fill='y')

        inner = tk.Frame(row, bg=bg)
        inner.pack(fill='x', padx=6, pady=4)

        tk.Label(inner, text=name, bg=bg, fg=_WG_TEXT,
                 font=get_cjk_font(9, True), anchor='w').pack(side='left', fill='x', expand=True)

        if installed:
            tk.Label(inner, text='✓', bg=bg, fg=_WG_GREEN,
                     font=get_cjk_font(9, True)).pack(side='right')
        else:
            lbl = tk.Label(inner, text='安装', bg=_WG_GOLD,
                           fg='#FFFFFF', font=get_cjk_font(8, True),
                           padx=6, pady=1, cursor='hand2')
            lbl.pack(side='right')
            lbl.bind('<Button-1>', lambda e, pid=pid: self._install(pid))

        row.bind('<Enter>', lambda e, r=row: r.configure(highlightbackground=_WG_GOLD))
        row.bind('<Leave>', lambda e, r=row: r.configure(highlightbackground=_WG_BORDER))

    def _install(self, plugin_id: str):
        def _do():
            try:
                from act_platform.runtime import act_plugin_import
                client = _get_workshop_client()
                if client is None:
                    return
                import tempfile
                detail = client.detail(plugin_id)
                meta = detail.get('plugin', {}) if isinstance(detail, dict) else {}
                with tempfile.TemporaryDirectory() as tmp:
                    path = client.download(plugin_id, tmp,
                                           expected_sha256=meta.get('sha256', ''))
                    act_plugin_import(self._owner, path)
                self._installed.append(plugin_id)
                self.after(0, self._filter_render)
            except Exception:
                pass
        threading.Thread(target=_do, daemon=True).start()

    def _open_full(self):
        if callable(self._on_open_full):
            self._on_open_full()


# ═══════════════════════════════════════════════════════════════════
#  WorkshopPanel — full floating panel with tabs
# ═══════════════════════════════════════════════════════════════════

class WorkshopPanel:
    """Full-featured workshop floating panel with tabs."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._canvas: Optional[tk.Canvas] = None
        self._grid_frame: Optional[tk.Frame] = None
        self._status_var = tk.StringVar(value='Ready')
        self._search_var = tk.StringVar()
        self._client = None
        self._plugins: list[dict] = []
        self._local_plugins: list[dict] = []
        self._installed: list[str] = []
        self._loading = False
        self._page = 1
        self._per_page = 30
        self._total = 0
        self._zoom = 1.0
        self._resize_start: Optional[tuple] = None
        self._active_tab = 'store'
        self._tab_buttons: dict[str, tk.Label] = {}
        self._tab_frames: dict[str, tk.Frame] = {}
        self._publish_menu_frame: Optional[tk.Frame] = None
        self._publish_var = tk.StringVar()
        self._publish_status: Optional[tk.Label] = None
        self._status_dot: Optional[tk.Label] = None
        self._zip_path_var = tk.StringVar()
        self._pub_id_var = tk.StringVar()
        self._pub_name_var = tk.StringVar()
        self._pub_version_var = tk.StringVar(value='1.0.0')
        self._pub_author_var = tk.StringVar()
        self._pub_desc_var = tk.StringVar()
        self._pub_games_var = tk.StringVar()
        self._pub_tags_var = tk.StringVar()
        self._pub_access_var = tk.StringVar(value='free')
        self._pub_long_desc: Optional[tk.Text] = None
        self._page_frame: Optional[tk.Frame] = None
        self._content: Optional[tk.Frame] = None
        self._canvas_win = None
        self._list_frame: Optional[tk.Frame] = None
        self._local_frame: Optional[tk.Frame] = None
        self._local_list: Optional[tk.Frame] = None

    def show(self) -> None:
        if self._win is None or not self._exists():
            self._build()
        if self._win is None:
            return
        try:
            self._win.deiconify()
            self._win.lift()
            self._win.attributes('-topmost', True)
            self._win.after(220, lambda: self._win and self._win.attributes('-topmost', False))
        except Exception:
            pass
        self._show_tab(self._active_tab)

    def hide(self) -> None:
        if self._win is not None:
            try:
                self._win.withdraw()
            except Exception:
                pass

    def destroy(self) -> None:
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists()
                    and self._win.state() != 'withdrawn')

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    # ── Build ──────────────────────────────────────────────────────

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO Creative Workshop')
        win.geometry('980x720+140+80')
        win.minsize(640, 420)
        win.configure(bg=_WG_BG)
        try:
            win.overrideredirect(True)
            win.attributes('-alpha', 0.98)
        except Exception:
            pass
        try:
            _apply_window_icon(win)
        except Exception:
            pass

        # ── Header ──
        header = tk.Frame(win, bg=_WG_HEADER_BG, height=44)
        header.pack(fill='x')
        header.pack_propagate(False)
        _bind_panel_drag(win, header)
        hdr_inner = tk.Frame(header, bg=_WG_HEADER_BG)
        hdr_inner.pack(fill='both', expand=True, padx=12, pady=0)
        tk.Label(hdr_inner, text='◇', bg=_WG_HEADER_BG, fg=_WG_GOLD,
                 font=get_sao_font(14, True)).pack(side='left')
        tk.Label(hdr_inner, text='CREATIVE WORKSHOP', bg=_WG_HEADER_BG,
                 fg=_WG_GOLD, font=get_sao_font(11, True)).pack(side='left', padx=(4, 0))
        tk.Label(hdr_inner, text='创意工坊', bg=_WG_HEADER_BG,
                 fg=_WG_HEADER_FG, font=get_cjk_font(10)).pack(side='left', padx=(8, 0))
        close_btn = tk.Label(hdr_inner, text='×', bg=_WG_HEADER_BG,
                             fg=_WG_MUTED, font=('Consolas', 16),
                             cursor='hand2', padx=6)
        close_btn.pack(side='right')
        close_btn.bind('<Button-1>', lambda e: self.hide())
        close_btn.bind('<Enter>', lambda e: close_btn.configure(fg=_WG_RED))
        close_btn.bind('<Leave>', lambda e: close_btn.configure(fg=_WG_MUTED))
        tk.Frame(win, bg=_WG_BORDER, height=1).pack(fill='x')

        # ── Tabs ──
        tab_bar = tk.Frame(win, bg=_WG_BODY_BG)
        tab_bar.pack(fill='x', padx=14, pady=(8, 0))
        for key, label in (('store', '在线商店'), ('local', '我的插件'), ('publish', '上传发布')):
            btn = _make_tab_btn(tab_bar, label, lambda k=key: self._show_tab(k))
            self._tab_buttons[key] = btn
        tk.Frame(win, bg=_WG_SEP, height=1).pack(fill='x', padx=14, pady=(6, 0))

        # ── Tab content frames ──
        content = tk.Frame(win, bg=_WG_BODY_BG)
        content.pack(fill='both', expand=True, padx=0, pady=0)
        self._content = content

        # Store tab
        store_frame = tk.Frame(content, bg=_WG_BODY_BG)
        self._tab_frames['store'] = store_frame
        self._build_store_tab(store_frame)

        # Local tab
        local_frame = tk.Frame(content, bg=_WG_BODY_BG)
        self._tab_frames['local'] = local_frame
        self._build_local_tab(local_frame)

        # Publish tab
        publish_frame = tk.Frame(content, bg=_WG_BODY_BG)
        self._tab_frames['publish'] = publish_frame
        self._build_publish_tab(publish_frame)

        # ── Status bar ──
        tk.Frame(win, bg=_WG_BORDER, height=1).pack(fill='x', pady=(4, 0))
        status_bar = tk.Frame(win, bg=_WG_HEADER_BG, height=28)
        status_bar.pack(fill='x')
        status_bar.pack_propagate(False)
        self._status_dot = tk.Label(status_bar, text='●', bg=_WG_HEADER_BG,
                                    fg=_WG_MUTED, font=('Consolas', 8))
        self._status_dot.pack(side='left', padx=(12, 4))
        tk.Label(status_bar, textvariable=self._status_var, bg=_WG_HEADER_BG,
                 fg=_WG_MUTED, font=get_cjk_font(9), anchor='w').pack(
                     side='left', fill='x', expand=True)
        self._page_frame = tk.Frame(status_bar, bg=_WG_HEADER_BG)
        self._page_frame.pack(side='right', padx=(0, 8))

        # ── Resize grip ──
        grip = tk.Label(win, text='⋱', bg=_WG_HEADER_BG, fg=_WG_BORDER,
                        font=('Consolas', 10), cursor='size_nw_se')
        grip.place(relx=1.0, rely=1.0, anchor='se', x=-2, y=-2)
        grip.bind('<ButtonPress-1>', self._resize_start_cb)
        grip.bind('<B1-Motion>', self._resize_motion_cb)

        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _build_store_tab(self, parent: tk.Frame):
        toolbar = tk.Frame(parent, bg=_WG_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(8, 4))
        search_frame = tk.Frame(toolbar, bg=_WG_CARD_BG, highlightthickness=1,
                                highlightbackground=_WG_BORDER)
        search_frame.pack(side='left', fill='x', expand=True)
        tk.Label(search_frame, text='⌕', bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(10)).pack(side='left', padx=(6, 0))
        entry = tk.Entry(search_frame, textvariable=self._search_var,
                         bg=_WG_CARD_BG, fg=_WG_TEXT, insertbackground=_WG_GOLD,
                         relief='flat', bd=0, highlightthickness=0,
                         font=get_cjk_font(10))
        entry.pack(side='left', fill='x', expand=True, padx=4, ipady=4)
        self._search_var.trace_add('write', lambda *_: self._on_search())

        zoom_frame = tk.Frame(toolbar, bg=_WG_BODY_BG)
        zoom_frame.pack(side='right', padx=(8, 0))
        for text, cmd in [('−', self._zoom_out), ('+', self._zoom_in)]:
            btn = tk.Label(zoom_frame, text=text, bg=_WG_CARD_BG,
                           fg=_WG_TEXT, font=get_cjk_font(12, True),
                           cursor='hand2', padx=8, pady=1, highlightthickness=1,
                           highlightbackground=_WG_BORDER)
            btn.pack(side='left', padx=2)
            btn.bind('<Button-1>', lambda e, c=cmd: c())
            btn.bind('<Enter>', lambda e, b=btn: b.configure(bg=_WG_GOLD_SOFT))
            btn.bind('<Leave>', lambda e, b=btn: b.configure(bg=_WG_CARD_BG))

        body = tk.Frame(parent, bg=_WG_BODY_BG)
        body.pack(fill='both', expand=True)
        canvas = tk.Canvas(body, bg=_WG_BODY_BG, highlightthickness=0, bd=0)
        scrollbar = tk.Scrollbar(body, orient='vertical', command=canvas.yview)
        self._grid_frame = tk.Frame(canvas, bg=_WG_BODY_BG)
        self._grid_frame.bind('<Configure>',
                              lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
        self._canvas_win = canvas.create_window((0, 0), window=self._grid_frame, anchor='nw')
        canvas.bind('<Configure>',
                    lambda e: canvas.itemconfigure(self._canvas_win, width=e.width))
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side='left', fill='both', expand=True, padx=(14, 0), pady=(4, 0))
        scrollbar.pack(side='right', fill='y', padx=(0, 4), pady=(4, 0))
        canvas.bind('<MouseWheel>', lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))
        self._canvas = canvas

    def _build_local_tab(self, parent: tk.Frame):
        toolbar = tk.Frame(parent, bg=_WG_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(8, 4))
        tk.Label(toolbar, text='用户安装的插件 (user_plugins/)', bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(9), anchor='w').pack(side='left')
        refresh_btn = tk.Label(toolbar, text='刷新', bg=_WG_CARD_BG, fg=_WG_TEXT,
                               font=get_cjk_font(9, True), padx=8, pady=2,
                               cursor='hand2', highlightthickness=1,
                               highlightbackground=_WG_BORDER)
        refresh_btn.pack(side='right')
        refresh_btn.bind('<Button-1>', lambda e: self._load_local())
        refresh_btn.bind('<Enter>', lambda e: refresh_btn.configure(bg=_WG_GOLD_SOFT))
        refresh_btn.bind('<Leave>', lambda e: refresh_btn.configure(bg=_WG_CARD_BG))

        import_btn = tk.Label(toolbar, text='导入 .zip', bg=_WG_GOLD, fg='#FFFFFF',
                              font=get_cjk_font(9, True), padx=8, pady=2,
                              cursor='hand2')
        import_btn.pack(side='right', padx=(0, 6))
        import_btn.bind('<Button-1>', lambda e: self._import_local())
        import_btn.bind('<Enter>', lambda e: import_btn.configure(bg=_WG_GOLD_DARK))
        import_btn.bind('<Leave>', lambda e: import_btn.configure(bg=_WG_GOLD))

        body = tk.Frame(parent, bg=_WG_BODY_BG)
        body.pack(fill='both', expand=True)
        canvas = tk.Canvas(body, bg=_WG_BODY_BG, highlightthickness=0, bd=0)
        scrollbar = tk.Scrollbar(body, orient='vertical', command=canvas.yview)
        self._local_list = tk.Frame(canvas, bg=_WG_BODY_BG)
        self._local_list.bind('<Configure>',
                              lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
        _lid = canvas.create_window((0, 0), window=self._local_list, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_lid, width=e.width))
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side='left', fill='both', expand=True, padx=(14, 0), pady=(4, 0))
        scrollbar.pack(side='right', fill='y', padx=(0, 4), pady=(4, 0))
        canvas.bind('<MouseWheel>', lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    def _build_publish_tab(self, parent: tk.Frame):
        outer = tk.Frame(parent, bg=_WG_BODY_BG)
        outer.pack(fill='both', expand=True)
        canvas = tk.Canvas(outer, bg=_WG_BODY_BG, highlightthickness=0)
        scrollbar = tk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        body = tk.Frame(canvas, bg=_WG_BODY_BG)
        body.bind('<Configure>', lambda _: canvas.configure(scrollregion=canvas.bbox('all')))
        _wid = canvas.create_window((0, 0), window=body, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_wid, width=e.width))
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side='left', fill='both', expand=True, padx=(14, 0), pady=10)
        scrollbar.pack(side='right', fill='y', padx=(0, 4), pady=10)
        canvas.bind('<MouseWheel>', lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

        lbl_font = get_cjk_font(9)
        entry_font = get_cjk_font(10)

        tk.Label(body, text='上传插件到创意工坊', bg=_WG_BODY_BG, fg=_WG_TEXT,
                 font=get_cjk_font(12, True), anchor='w').pack(fill='x', pady=(0, 4))
        tk.Label(body, text='填写插件信息并选择 .zip 包上传，其他用户即可浏览和下载。',
                 bg=_WG_BODY_BG, fg=_WG_MUTED, font=lbl_font,
                 anchor='w', wraplength=600).pack(fill='x', pady=(0, 12))

        tk.Frame(body, bg=_WG_SEP, height=1).pack(fill='x', pady=(0, 10))

        def _field(parent, label_text, var, width=50):
            row = tk.Frame(parent, bg=_WG_BODY_BG)
            row.pack(fill='x', pady=(0, 6))
            tk.Label(row, text=label_text, bg=_WG_BODY_BG, fg=_WG_MUTED,
                     font=lbl_font, width=12, anchor='e').pack(side='left', padx=(0, 6))
            e = tk.Entry(row, textvariable=var, bg=_WG_CARD_BG, fg=_WG_TEXT,
                         font=entry_font, relief='flat', highlightthickness=1,
                         highlightbackground=_WG_BORDER, insertbackground=_WG_TEXT,
                         width=width)
            e.pack(side='left', fill='x', expand=True)
            return e

        # Select from local plugins
        sel_frame = tk.Frame(body, bg=_WG_BODY_BG)
        sel_frame.pack(fill='x', pady=(0, 8))
        tk.Label(sel_frame, text='选择插件:', bg=_WG_BODY_BG, fg=_WG_TEXT,
                 font=get_cjk_font(10), anchor='w').pack(side='left')
        self._publish_var = tk.StringVar()
        self._publish_menu_frame = tk.Frame(sel_frame, bg=_WG_BODY_BG)
        self._publish_menu_frame.pack(side='left', padx=(8, 0))
        fill_btn = tk.Label(sel_frame, text='填入', bg=_WG_CARD_BG, fg=_WG_GOLD,
                            font=get_cjk_font(9, True), padx=8, pady=2, cursor='hand2',
                            highlightthickness=1, highlightbackground=_WG_BORDER)
        fill_btn.pack(side='left', padx=(8, 0))
        fill_btn.bind('<Button-1>', lambda e: self._fill_from_selected())

        tk.Frame(body, bg=_WG_SEP, height=1).pack(fill='x', pady=(4, 8))

        # Metadata fields
        self._pub_id_var = tk.StringVar()
        self._pub_name_var = tk.StringVar()
        self._pub_version_var = tk.StringVar(value='1.0.0')
        self._pub_author_var = tk.StringVar()
        self._pub_desc_var = tk.StringVar()
        self._pub_games_var = tk.StringVar()
        self._pub_tags_var = tk.StringVar()

        _field(body, '显示名称', self._pub_name_var)

        row2 = tk.Frame(body, bg=_WG_BODY_BG)
        row2.pack(fill='x', pady=(0, 6))
        tk.Label(row2, text='版本', bg=_WG_BODY_BG, fg=_WG_MUTED,
                 font=lbl_font, width=12, anchor='e').pack(side='left', padx=(0, 6))
        tk.Entry(row2, textvariable=self._pub_version_var, bg=_WG_CARD_BG, fg=_WG_TEXT,
                 font=entry_font, relief='flat', highlightthickness=1,
                 highlightbackground=_WG_BORDER, insertbackground=_WG_TEXT,
                 width=15).pack(side='left')
        tk.Label(row2, text='作者', bg=_WG_BODY_BG, fg=_WG_MUTED,
                 font=lbl_font, anchor='e').pack(side='left', padx=(16, 6))
        tk.Entry(row2, textvariable=self._pub_author_var, bg=_WG_CARD_BG, fg=_WG_TEXT,
                 font=entry_font, relief='flat', highlightthickness=1,
                 highlightbackground=_WG_BORDER, insertbackground=_WG_TEXT,
                 width=20).pack(side='left', fill='x', expand=True)

        _field(body, '简介', self._pub_desc_var)

        # Long description
        tk.Label(body, text='详细介绍 (可选)', bg=_WG_BODY_BG, fg=_WG_MUTED,
                 font=lbl_font, anchor='w').pack(fill='x', padx=(0, 0), pady=(0, 2))
        self._pub_long_desc = tk.Text(body, bg=_WG_CARD_BG, fg=_WG_TEXT,
                                       font=entry_font, relief='flat', height=4,
                                       highlightthickness=1, highlightbackground=_WG_BORDER,
                                       insertbackground=_WG_TEXT, wrap='word')
        self._pub_long_desc.pack(fill='x', pady=(0, 6))

        _field(body, '游戏 (逗号隔)', self._pub_games_var)
        _field(body, '标签 (逗号隔)', self._pub_tags_var)

        # Access level
        acc_frame = tk.Frame(body, bg=_WG_BODY_BG)
        acc_frame.pack(fill='x', pady=(0, 6))
        tk.Label(acc_frame, text='访问权限', bg=_WG_BODY_BG, fg=_WG_MUTED,
                 font=lbl_font, width=12, anchor='e').pack(side='left', padx=(0, 6))
        self._pub_access_var = tk.StringVar(value='free')
        tk.Radiobutton(acc_frame, text='免费 (所有用户)', variable=self._pub_access_var,
                       value='free', bg=_WG_BODY_BG, fg=_WG_TEXT, selectcolor=_WG_CARD_BG,
                       font=lbl_font, activebackground=_WG_BODY_BG,
                       activeforeground=_WG_TEXT).pack(side='left', padx=(0, 12))
        tk.Radiobutton(acc_frame, text='★ 高级版专属 (付费用户)', variable=self._pub_access_var,
                       value='paid', bg=_WG_BODY_BG, fg='#ffc040', selectcolor=_WG_CARD_BG,
                       font=lbl_font, activebackground=_WG_BODY_BG,
                       activeforeground='#ffc040').pack(side='left')

        tk.Frame(body, bg=_WG_SEP, height=1).pack(fill='x', pady=(4, 8))

        # Zip file selector
        or_frame = tk.Frame(body, bg=_WG_BODY_BG)
        or_frame.pack(fill='x', pady=(0, 8))
        tk.Label(or_frame, text='.zip 文件', bg=_WG_BODY_BG, fg=_WG_MUTED,
                 font=lbl_font, width=12, anchor='e').pack(side='left', padx=(0, 6))
        self._zip_path_var = tk.StringVar()
        tk.Entry(or_frame, textvariable=self._zip_path_var, bg=_WG_CARD_BG, fg=_WG_TEXT,
                 font=entry_font, relief='flat', highlightthickness=1,
                 highlightbackground=_WG_BORDER, insertbackground=_WG_TEXT,
                 state='readonly', width=40).pack(side='left', fill='x', expand=True, padx=(0, 4))
        browse_btn = tk.Label(or_frame, text='浏览', bg=_WG_CARD_BG, fg=_WG_TEXT,
                              font=get_cjk_font(9, True), padx=8, pady=2,
                              cursor='hand2', highlightthickness=1,
                              highlightbackground=_WG_BORDER)
        browse_btn.pack(side='left')
        browse_btn.bind('<Button-1>', lambda e: self._browse_zip())

        tk.Frame(body, bg=_WG_SEP, height=1).pack(fill='x', pady=(4, 10))

        pub_btn = tk.Label(body, text='  上传到创意工坊  ', bg=_WG_GOLD, fg='#FFFFFF',
                           font=get_cjk_font(11, True), padx=16, pady=6, cursor='hand2')
        pub_btn.pack(anchor='w')
        pub_btn.bind('<Button-1>', lambda e: self._do_publish())
        pub_btn.bind('<Enter>', lambda e: pub_btn.configure(bg=_WG_GOLD_DARK))
        pub_btn.bind('<Leave>', lambda e: pub_btn.configure(bg=_WG_GOLD))

        self._publish_status = tk.Label(body, text='', bg=_WG_BODY_BG, fg=_WG_MUTED,
                                        font=get_cjk_font(9), anchor='w', wraplength=500)
        self._publish_status.pack(fill='x', pady=(8, 0))

    # ── Tab switching ─────────────────────────────────────────────

    def _show_tab(self, name: str):
        self._active_tab = name
        _set_tab_active(self._tab_buttons, name)
        for key, frame in self._tab_frames.items():
            if key == name:
                frame.pack(fill='both', expand=True)
            else:
                frame.pack_forget()
        if name == 'store':
            self._load_catalog()
        elif name == 'local':
            self._load_local()
        elif name == 'publish':
            self._refresh_publish_menu()

    # ── Store tab ─────────────────────────────────────────────────

    def _on_search(self):
        self._page = 1
        self._load_catalog()

    def _zoom_in(self):
        self._zoom = min(1.5, self._zoom + 0.1)
        self._render_grid(self._plugins)

    def _zoom_out(self):
        self._zoom = max(0.7, self._zoom - 0.1)
        self._render_grid(self._plugins)

    def _load_catalog(self):
        if self._loading:
            return
        self._loading = True
        self._render_loading()
        self._installed = _get_installed_ids(self.owner)

        def _fetch():
            client = _get_workshop_client()
            if client is None:
                self.root.after(0, lambda: self._on_error('Workshop server unavailable'))
                self._loading = False
                return
            try:
                query = self._search_var.get().strip()
                result = client.catalog(search=query, page=self._page,
                                        per_page=self._per_page)
                if isinstance(result, dict) and result.get('ok') is not False:
                    plugins = result.get('plugins') or []
                    total = _finite_int(result.get('total'), len(plugins), lo=0)
                    self.root.after(0, lambda p=plugins, t=total:
                                   self._on_loaded(p, t))
                else:
                    err = str((result or {}).get('error', 'Unknown error'))
                    self.root.after(0, lambda e=err: self._on_error(e))
            except Exception as exc:
                self.root.after(0, lambda e=str(exc): self._on_error(e))
            self._loading = False

        threading.Thread(target=_fetch, daemon=True).start()

    def _on_loaded(self, plugins: list, total: int):
        self._loading = False
        self._plugins = list(plugins)
        self._total = total
        if self._status_dot is not None:
            self._status_dot.configure(fg=_WG_GREEN)
        self._status_var.set(f'{total} plugins available')
        self._render_grid(plugins)
        self._render_pagination()

    def _on_error(self, msg: str):
        self._loading = False
        if self._status_dot is not None:
            self._status_dot.configure(fg=_WG_RED)
        self._status_var.set(str(msg))
        self._render_empty(msg)

    def _render_loading(self):
        if self._grid_frame is None:
            return
        for w in self._grid_frame.winfo_children():
            w.destroy()
        tk.Label(self._grid_frame, text='加载中...', bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(11)).pack(pady=40)

    def _render_empty(self, msg: str = ''):
        if self._grid_frame is None:
            return
        for w in self._grid_frame.winfo_children():
            w.destroy()
        tk.Label(self._grid_frame, text='◇', bg=_WG_BODY_BG,
                 fg=_WG_BORDER, font=get_sao_font(36)).pack(pady=(30, 8))
        tk.Label(self._grid_frame, text=msg or '无插件', bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(11)).pack()

    def _render_grid(self, plugins: list):
        if self._grid_frame is None:
            return
        for w in self._grid_frame.winfo_children():
            w.destroy()
        if not plugins:
            self._render_empty('无结果')
            return
        z = self._zoom
        cols = max(1, _COLS)
        card_w = max(180, int(_CARD_W * z))
        for idx, plugin in enumerate(plugins):
            self._render_store_card(self._grid_frame, plugin, idx // cols, idx % cols, card_w, z)
        for c in range(cols):
            self._grid_frame.columnconfigure(c, weight=1, uniform='wscard')

    def _render_store_card(self, parent, plugin, row, col, card_w, z):
        pid = str(plugin.get('plugin_id') or '')
        name = str(plugin.get('name') or pid)
        author = str(plugin.get('author') or 'Unknown')
        desc = str(plugin.get('description') or '')[:80]
        installed = pid in self._installed
        downloads = _finite_int(plugin.get('download_count'), 0, lo=0)
        access = str(plugin.get('access_level') or 'free')
        base_font = max(8, int(9 * z))
        title_font = max(9, int(11 * z))

        card = tk.Frame(parent, bg=_WG_CARD_BG, highlightthickness=1,
                        highlightbackground=_WG_BORDER, cursor='hand2')
        card.grid(row=row, column=col, padx=_CARD_PAD, pady=_CARD_PAD, sticky='nsew')
        inner = tk.Frame(card, bg=_WG_CARD_BG)
        inner.pack(fill='both', expand=True, padx=int(10 * z), pady=int(8 * z))

        top = tk.Frame(inner, bg=_WG_CARD_BG)
        top.pack(fill='x', pady=(0, 4))
        tk.Label(top, text='◇', bg=_WG_BADGE_BG, fg=_WG_GOLD,
                 font=get_sao_font(int(14 * z)), width=2, relief='flat').pack(side='left', padx=(0, 8))
        title_area = tk.Frame(top, bg=_WG_CARD_BG)
        title_area.pack(side='left', fill='x', expand=True)
        tk.Label(title_area, text=name, bg=_WG_CARD_BG, fg=_WG_TEXT,
                 font=get_cjk_font(title_font, True), anchor='w').pack(fill='x')
        tk.Label(title_area, text=f'by {author}', bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(base_font), anchor='w').pack(fill='x')

        tk.Label(inner, text=desc, bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(base_font), anchor='w', justify='left',
                 wraplength=max(160, card_w - 40)).pack(fill='x', pady=(2, 6))

        footer = tk.Frame(inner, bg=_WG_CARD_BG)
        footer.pack(fill='x')
        badge_font = get_cjk_font(max(7, int(8 * z)))
        tk.Label(footer, text=f'↓ {downloads}', bg=_WG_BADGE_BG, fg=_WG_MUTED,
                 font=badge_font, padx=4, pady=1).pack(side='left', padx=(0, 4))
        if access == 'paid':
            tk.Label(footer, text='★ PREMIUM', bg='#3a2010', fg='#ffc040',
                     font=badge_font, padx=6, pady=1).pack(side='left', padx=(0, 4))
        is_paid_user = _is_paid()
        if installed:
            tk.Label(footer, text='✓ 已安装', bg='#E8F5E9', fg=_WG_GREEN,
                     font=badge_font, padx=6, pady=1).pack(side='right')
        elif access == 'paid' and not is_paid_user:
            tk.Label(footer, text='高级版专属', bg='#2a1a10', fg='#c09050',
                     font=badge_font, padx=8, pady=2).pack(side='right')
        else:
            install_btn = tk.Label(footer, text='安装', bg=_WG_GOLD, fg='#FFFFFF',
                                   font=get_cjk_font(max(7, int(8 * z)), True),
                                   padx=8, pady=2, cursor='hand2')
            install_btn.pack(side='right')
            install_btn.bind('<Button-1>', lambda e, p=pid: self._install_plugin(p))
            install_btn.bind('<Enter>', lambda e, b=install_btn: b.configure(bg=_WG_GOLD_DARK))
            install_btn.bind('<Leave>', lambda e, b=install_btn: b.configure(bg=_WG_GOLD))

        def _enter(e, c=card): c.configure(highlightbackground=_WG_GOLD)
        def _leave(e, c=card): c.configure(highlightbackground=_WG_BORDER)
        for widget in (card, inner, top, footer):
            widget.bind('<Enter>', _enter)
            widget.bind('<Leave>', _leave)

    def _render_pagination(self):
        if self._page_frame is None:
            return
        for w in self._page_frame.winfo_children():
            w.destroy()
        total_pages = max(1, math.ceil(self._total / max(1, self._per_page)))
        if total_pages <= 1:
            return
        def _make_btn(text, page, active=False):
            bg = _WG_GOLD if active else _WG_CARD_BG
            fg = '#FFFFFF' if active else _WG_TEXT
            btn = tk.Label(self._page_frame, text=str(text), bg=bg, fg=fg,
                           font=get_cjk_font(8), padx=6, pady=1, cursor='hand2',
                           highlightthickness=1, highlightbackground=_WG_BORDER)
            btn.pack(side='left', padx=1)
            if not active:
                btn.bind('<Button-1>', lambda e, p=page: self._go_page(p))
                btn.bind('<Enter>', lambda e, b=btn: b.configure(bg=_WG_GOLD_SOFT))
                btn.bind('<Leave>', lambda e, b=btn: b.configure(bg=_WG_CARD_BG))
        if self._page > 1:
            _make_btn('<', self._page - 1)
        for i in range(1, min(total_pages + 1, 8)):
            _make_btn(i, i, active=(i == self._page))
        if self._page < total_pages:
            _make_btn('>', self._page + 1)

    def _go_page(self, page: int):
        self._page = page
        self._load_catalog()
        if self._canvas:
            self._canvas.yview_moveto(0)

    def _install_plugin(self, plugin_id: str):
        self._status_var.set(f'Installing {plugin_id}...')
        def _do():
            try:
                client = _get_workshop_client()
                if client is None:
                    self.root.after(0, lambda: self._status_var.set('Client unavailable'))
                    return
                detail = client.detail(plugin_id)
                meta = detail.get('plugin', {}) if isinstance(detail, dict) else {}
                import tempfile
                with tempfile.TemporaryDirectory() as tmp:
                    path = client.download(plugin_id, tmp,
                                           expected_sha256=meta.get('sha256', ''))
                    try:
                        from act_platform.runtime import act_plugin_import
                        act_plugin_import(self.owner, path)
                    except ImportError:
                        from act_platform.plugin_install import install_plugin_archive
                        from config import BASE_DIR
                        dest = os.path.join(BASE_DIR, 'user_plugins')
                        os.makedirs(dest, exist_ok=True)
                        install_plugin_archive(path, dest)
                if plugin_id not in self._installed:
                    self._installed.append(plugin_id)
                self.root.after(0, lambda: (
                    self._status_var.set(f'Installed {plugin_id}'),
                    self._render_grid(self._plugins),
                ))
            except Exception as exc:
                self.root.after(0, lambda e=str(exc): self._status_var.set(f'Install failed: {e}'))
        threading.Thread(target=_do, daemon=True).start()

    # ── Local tab ─────────────────────────────────────────────────

    def _load_local(self):
        self._local_plugins = _get_local_user_plugins()
        self._render_local_list()

    def _render_local_list(self):
        parent = getattr(self, '_local_list', None)
        if parent is None:
            return
        for w in parent.winfo_children():
            w.destroy()
        if not self._local_plugins:
            tk.Label(parent, text='◇', bg=_WG_BODY_BG, fg=_WG_BORDER,
                     font=get_sao_font(36)).pack(pady=(30, 8))
            tk.Label(parent, text='无用户插件', bg=_WG_BODY_BG,
                     fg=_WG_MUTED, font=get_cjk_font(11)).pack()
            tk.Label(parent, text='使用「导入 .zip」按钮安装插件，或从在线商店下载。',
                     bg=_WG_BODY_BG, fg=_WG_MUTED, font=get_cjk_font(9),
                     wraplength=400).pack(pady=(4, 0))
            return
        self._status_var.set(f'{len(self._local_plugins)} user plugins')
        for idx, plugin in enumerate(self._local_plugins):
            self._render_local_card(parent, plugin, idx)

    def _render_local_card(self, parent, plugin: dict, idx: int):
        pid = str(plugin.get('id') or '')
        name = str(plugin.get('name') or pid)
        version = str(plugin.get('version') or '')
        author = str(plugin.get('author') or '')
        desc = str(plugin.get('description') or '')[:100]

        card = tk.Frame(parent, bg=_WG_CARD_BG, highlightthickness=1,
                        highlightbackground=_WG_BORDER)
        card.pack(fill='x', padx=14, pady=4)

        rail = tk.Frame(card, bg=_WG_GREEN, width=3)
        rail.pack(side='left', fill='y')

        inner = tk.Frame(card, bg=_WG_CARD_BG)
        inner.pack(fill='x', padx=12, pady=8)

        # Top row: name + version
        top = tk.Frame(inner, bg=_WG_CARD_BG)
        top.pack(fill='x')
        tk.Label(top, text=name, bg=_WG_CARD_BG, fg=_WG_TEXT,
                 font=get_cjk_font(11, True), anchor='w').pack(side='left')
        if version:
            tk.Label(top, text=f'v{version}', bg=_WG_CARD_BG, fg=_WG_MUTED,
                     font=get_cjk_font(8), anchor='e').pack(side='right')

        if author:
            tk.Label(inner, text=f'by {author}', bg=_WG_CARD_BG, fg=_WG_MUTED,
                     font=get_cjk_font(9), anchor='w').pack(fill='x')
        if desc:
            tk.Label(inner, text=desc, bg=_WG_CARD_BG, fg=_WG_MUTED,
                     font=get_cjk_font(9), anchor='w', wraplength=600).pack(fill='x', pady=(2, 0))

        tk.Frame(inner, bg=_WG_SEP, height=1).pack(fill='x', pady=(6, 4))

        # Action buttons
        actions = tk.Frame(inner, bg=_WG_CARD_BG)
        actions.pack(fill='x')

        # Open in AI editor
        ed_btn = tk.Label(actions, text='✦ AI Editor', bg=_WG_BADGE_BG, fg=_WG_GOLD,
                          font=get_cjk_font(9, True), padx=8, pady=2, cursor='hand2')
        ed_btn.pack(side='left', padx=(0, 4))
        ed_btn.bind('<Button-1>', lambda e, p=plugin: self._open_in_editor(p))
        ed_btn.bind('<Enter>', lambda e: ed_btn.configure(bg=_WG_GOLD_SOFT))
        ed_btn.bind('<Leave>', lambda e: ed_btn.configure(bg=_WG_BADGE_BG))

        # Upload to workshop
        pub_btn = tk.Label(actions, text='↑ 上传', bg=_WG_BADGE_BG, fg=_WG_GOLD,
                           font=get_cjk_font(9, True), padx=8, pady=2, cursor='hand2')
        pub_btn.pack(side='left', padx=(0, 4))
        pub_btn.bind('<Button-1>', lambda e, p=plugin: self._publish_local(p))
        pub_btn.bind('<Enter>', lambda e: pub_btn.configure(bg=_WG_GOLD_SOFT))
        pub_btn.bind('<Leave>', lambda e: pub_btn.configure(bg=_WG_BADGE_BG))

        # Delete
        del_btn = tk.Label(actions, text='删除', bg='#FFF0F0', fg=_WG_RED,
                           font=get_cjk_font(9, True), padx=8, pady=2, cursor='hand2')
        del_btn.pack(side='right')
        del_btn.bind('<Button-1>', lambda e, p=pid: self._delete_local(p))
        del_btn.bind('<Enter>', lambda e: del_btn.configure(bg='#FFE0E0'))
        del_btn.bind('<Leave>', lambda e: del_btn.configure(bg='#FFF0F0'))

        card.bind('<Enter>', lambda e, c=card: c.configure(highlightbackground=_WG_GOLD))
        card.bind('<Leave>', lambda e, c=card: c.configure(highlightbackground=_WG_BORDER))

    def _open_in_editor(self, plugin: dict):
        _open_ai_editor_via_owner(self.owner, status_var=self._status_var)

    def _delete_local(self, plugin_id: str):
        try:
            from act_platform.runtime import act_plugin_uninstall
            result = act_plugin_uninstall(self.owner, plugin_id)
            msg = str(result.get('message') or ('已删除' if result.get('ok') else '删除失败'))
            self._status_var.set(msg)
        except Exception as exc:
            self._status_var.set(f'删除失败: {exc}')
        self._load_local()

    def _import_local(self):
        try:
            path = filedialog.askopenfilename(
                parent=self._win,
                title='导入插件 Import plugin',
                filetypes=(('SAO 插件包', '*.zip *.saoplugin'), ('All files', '*.*')),
            )
        except Exception:
            return
        if not path:
            return
        try:
            from act_platform.runtime import act_plugin_import
            result = act_plugin_import(self.owner, str(path))
            msg = str(result.get('message') or ('已导入' if result.get('ok') else '导入失败'))
            self._status_var.set(msg)
        except Exception as exc:
            self._status_var.set(f'导入失败: {exc}')
        self._load_local()

    def _publish_local(self, plugin: dict, form_meta: dict | None = None):
        plugin_path = str(plugin.get('path') or '')
        if not plugin_path or not os.path.isdir(plugin_path):
            self._publish_status.configure(text='插件目录不存在', fg=_WG_RED)
            return
        self._publish_status.configure(text=f'正在打包 {plugin.get("name")}...', fg=_WG_GOLD)

        def _do():
            try:
                import shutil
                import tempfile
                with tempfile.TemporaryDirectory() as tmp:
                    zip_base = os.path.join(tmp, str(plugin.get('id') or 'plugin'))
                    zip_path = shutil.make_archive(zip_base, 'zip', plugin_path)
                    client = _get_workshop_client()
                    if client is None:
                        self.root.after(0, lambda: self._publish_status.configure(
                            text='Workshop server unavailable', fg=_WG_RED))
                        return
                    meta = {}
                    meta_path = os.path.join(plugin_path, 'plugin.json')
                    if os.path.isfile(meta_path):
                        with open(meta_path, 'r', encoding='utf-8') as f:
                            meta = json.load(f)
                    if form_meta:
                        for k, v in form_meta.items():
                            if v:
                                meta[k] = v
                    result = client.publish(zip_path, meta)
                    ok = result.get('ok', False) if isinstance(result, dict) else False
                    name = ''
                    if isinstance(result, dict) and isinstance(result.get('plugin'), dict):
                        name = result['plugin'].get('name', '')
                    msg = f'上传成功! {name}' if ok else str(result.get('error', '上传失败'))
                    color = _WG_GREEN if ok else _WG_RED
                    self.root.after(0, lambda m=msg, c=color:
                                   self._publish_status.configure(text=m, fg=c))
            except Exception as exc:
                self.root.after(0, lambda e=str(exc):
                               self._publish_status.configure(text=f'上传失败: {e}', fg=_WG_RED))

        threading.Thread(target=_do, daemon=True).start()

    # ── Publish tab ───────────────────────────────────────────────

    def _refresh_publish_menu(self):
        self._local_plugins = _get_local_user_plugins()
        parent = self._publish_menu_frame
        if parent is None:
            return
        for w in parent.winfo_children():
            w.destroy()
        if not self._local_plugins:
            tk.Label(parent, text='(无用户插件)', bg=_WG_BODY_BG, fg=_WG_MUTED,
                     font=get_cjk_font(9)).pack(side='left')
            return
        names = [f'{p["name"]} ({p["id"]})' for p in self._local_plugins]
        self._publish_var.set(names[0] if names else '')
        try:
            om = tk.OptionMenu(parent, self._publish_var, *names)
            om.configure(bg=_WG_CARD_BG, fg=_WG_TEXT, font=get_cjk_font(9),
                         highlightthickness=1, highlightbackground=_WG_BORDER,
                         relief='flat')
            om.pack(side='left')
        except Exception:
            pass

    def _fill_from_selected(self):
        selected = self._publish_var.get().strip()
        if not selected:
            return
        plugin = None
        for p in self._local_plugins:
            label = f'{p["name"]} ({p["id"]})'
            if label == selected:
                plugin = p
                break
        if plugin is None:
            return
        self._pub_id_var.set(plugin.get('id', ''))  # hidden, auto-generated if empty
        self._pub_name_var.set(plugin.get('name', ''))
        self._pub_version_var.set(plugin.get('version', '1.0.0'))
        self._pub_desc_var.set(plugin.get('description', ''))
        self._pub_games_var.set(', '.join(plugin.get('game_ids', [])))
        self._pub_author_var.set(plugin.get('author', ''))

    def _collect_publish_meta(self) -> dict:
        def _split(s): return [x.strip() for x in s.split(',') if x.strip()]
        long_desc = ''
        try:
            long_desc = self._pub_long_desc.get('1.0', 'end-1c').strip()
        except Exception:
            pass
        plugin_name = self._pub_name_var.get().strip()
        pid = self._pub_id_var.get().strip()
        if not pid and plugin_name:
            try:
                from workshop.app import generate_plugin_id
                pid = generate_plugin_id(plugin_name)
            except Exception:
                pid = plugin_name.lower().replace(' ', '_')
        return {
            'plugin_id': pid,
            'name': self._pub_name_var.get().strip(),
            'version': self._pub_version_var.get().strip() or '1.0.0',
            'author': self._pub_author_var.get().strip(),
            'description': self._pub_desc_var.get().strip(),
            'long_description': long_desc,
            'game_ids': _split(self._pub_games_var.get()),
            'tags': _split(self._pub_tags_var.get()),
            'access_level': self._pub_access_var.get().strip() or 'free',
        }

    def _browse_zip(self):
        try:
            path = filedialog.askopenfilename(
                parent=self._win,
                title='选择插件包',
                filetypes=(('ZIP files', '*.zip'), ('All files', '*.*')),
            )
            if path:
                self._zip_path_var.set(path)
        except Exception:
            pass

    def _do_publish(self):
        meta = self._collect_publish_meta()
        if not meta.get('name') and not meta.get('plugin_id'):
            self._publish_status.configure(text='请填写插件名称', fg=_WG_RED)
            return
        if meta.get('access_level') == 'paid' and not _is_paid():
            self._publish_status.configure(text='只有付费用户才能上传高级版专属插件', fg=_WG_RED)
            return

        zip_path = self._zip_path_var.get().strip()
        if zip_path and os.path.isfile(zip_path):
            self._publish_zip(zip_path, meta)
            return

        selected = self._publish_var.get().strip()
        if not selected:
            self._publish_status.configure(text='请选择一个插件或 .zip 文件', fg=_WG_RED)
            return
        plugin = None
        for p in self._local_plugins:
            label = f'{p["name"]} ({p["id"]})'
            if label == selected:
                plugin = p
                break
        if plugin is None:
            self._publish_status.configure(text='未找到所选插件', fg=_WG_RED)
            return
        self._publish_local(plugin, meta)

    def _publish_zip(self, zip_path: str, meta: dict | None = None):
        if meta is None:
            meta = self._collect_publish_meta()
        self._publish_status.configure(text='正在上传...', fg=_WG_GOLD)

        def _do():
            try:
                client = _get_workshop_client()
                if client is None:
                    self.root.after(0, lambda: self._publish_status.configure(
                        text='Workshop server unavailable', fg=_WG_RED))
                    return
                result = client.publish(zip_path, meta)
                ok = result.get('ok', False) if isinstance(result, dict) else False
                name = ''
                if isinstance(result, dict) and isinstance(result.get('plugin'), dict):
                    name = result['plugin'].get('name', '')
                msg = f'上传成功! {name}' if ok else str(result.get('error', '上传失败'))
                color = _WG_GREEN if ok else _WG_RED
                self.root.after(0, lambda m=msg, c=color:
                               self._publish_status.configure(text=m, fg=c))
            except Exception as exc:
                self.root.after(0, lambda e=str(exc):
                               self._publish_status.configure(text=f'上传失败: {e}', fg=_WG_RED))

        threading.Thread(target=_do, daemon=True).start()

    # ── Resize ────────────────────────────────────────────────────

    def _resize_start_cb(self, event):
        self._resize_start = (event.x_root, event.y_root,
                              self._win.winfo_width(), self._win.winfo_height())

    def _resize_motion_cb(self, event):
        if self._resize_start is None:
            return
        sx, sy, sw, sh = self._resize_start
        new_w = max(640, sw + (event.x_root - sx))
        new_h = max(420, sh + (event.y_root - sy))
        x = self._win.winfo_x()
        y = self._win.winfo_y()
        self._win.geometry(f'{new_w}x{new_h}+{x}+{y}')


__all__ = ['WorkshopPanel', 'WorkshopChildPreview']
