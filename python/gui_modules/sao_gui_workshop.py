# -*- coding: utf-8 -*-
"""Creative Workshop — SAO menu sidebar category + floating panel.

Two widgets:
  * ``WorkshopChildPreview``: compact preview inside the SAO menu childbar area
    (search + featured list + "open full panel" button).
  * ``WorkshopPanel``: full-featured floating Tk Toplevel with plugin card grid,
    search, resize, drag, zoom, install — white+gold colour scheme.

Both use ``workshop.client.WorkshopClient`` for catalog data (HTTP, threaded).
"""

from __future__ import annotations

import math
import threading
import tkinter as tk
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


def _get_workshop_client():
    try:
        from workshop.client import WorkshopClient
        from workshop.app import _get_server_url
        return WorkshopClient(base_url=_get_server_url())
    except Exception:
        return None


def _get_installed_ids(owner: Any) -> list[str]:
    try:
        from act_platform.runtime import act_plugin_status
        status = act_plugin_status(owner)
        return [str(p.get('id') or '') for p in (status.get('plugins') or [])]
    except Exception:
        pass
    try:
        import os
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
        self._installed: list[str] = []
        self._loading = False
        self._loaded = False
        self._search_var = tk.StringVar()
        self._build()

    def _build(self):
        bg = _WG_BODY_BG
        self.configure(bg=bg, width=self._PREVIEW_W)

        # ── Header: diamond accent + title ──
        title_f = tk.Frame(self, bg=bg)
        title_f.pack(fill='x', padx=12, pady=(10, 0))
        hdr_row = tk.Frame(title_f, bg=bg)
        hdr_row.pack(fill='x')
        tk.Label(hdr_row, text='◇', bg=bg, fg=_WG_GOLD,
                 font=get_sao_font(12, True)).pack(side='left', padx=(0, 6))
        tk.Label(hdr_row, text='WORKSHOP', bg=bg, fg=_WG_GOLD,
                 font=get_sao_font(9, True), anchor='w').pack(side='left')
        tk.Label(title_f, text='创意工坊', bg=bg, fg=_WG_MUTED,
                 font=get_cjk_font(9), anchor='w').pack(fill='x', padx=(0, 0), pady=(1, 0))

        # ── Gold accent line ──
        tk.Frame(self, bg=_WG_GOLD, height=1).pack(fill='x', padx=12, pady=(6, 0))
        tk.Frame(self, bg=_WG_SEP, height=1).pack(fill='x', padx=12, pady=(1, 6))

        # ── Search ──
        search_f = tk.Frame(self, bg=bg)
        search_f.pack(fill='x', padx=12, pady=(0, 6))
        search_inner = tk.Frame(search_f, bg=_WG_CARD_BG, highlightthickness=1,
                                highlightbackground=_WG_BORDER)
        search_inner.pack(fill='x')
        tk.Label(search_inner, text='⌕', bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(10)).pack(side='left', padx=(6, 0))
        entry = tk.Entry(search_inner, textvariable=self._search_var,
                         bg=_WG_CARD_BG, fg=_WG_TEXT, insertbackground=_WG_GOLD,
                         relief='flat', bd=0, highlightthickness=0,
                         font=get_cjk_font(9))
        entry.pack(side='left', fill='x', expand=True, padx=4, ipady=4)
        self._search_var.trace_add('write', lambda *_: self._filter_render())

        # ── Plugin list area ──
        self._list_frame = tk.Frame(self, bg=bg)
        self._list_frame.pack(fill='both', expand=True, padx=12, pady=(0, 4))

        # ── Bottom: separator + button ──
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
        if not self._loaded:
            self._load_featured()

    def _load_featured(self):
        if self._loading:
            return
        self._loading = True
        self._render_loading()
        self._installed = _get_installed_ids(self._owner)

        def _fetch():
            client = _get_workshop_client()
            if client is None:
                self.after(0, lambda: self._render_error('Workshop client unavailable'))
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
        self._render_list(plugins[:6])

    def _render_loading(self):
        for w in self._list_frame.winfo_children():
            w.destroy()
        tk.Label(self._list_frame, text='加载中...', bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(9)).pack(pady=12)

    def _render_error(self, msg: str):
        self._loading = False
        for w in self._list_frame.winfo_children():
            w.destroy()
        tk.Label(self._list_frame, text='离线', bg=_WG_BODY_BG,
                 fg=_WG_RED, font=get_cjk_font(9)).pack(pady=4)
        tk.Label(self._list_frame, text=str(msg)[:40], bg=_WG_BODY_BG,
                 fg=_WG_MUTED, font=get_cjk_font(8), wraplength=220).pack(pady=2)

    def _render_list(self, plugins: list):
        for w in self._list_frame.winfo_children():
            w.destroy()
        if not plugins:
            tk.Label(self._list_frame, text='无结果', bg=_WG_BODY_BG,
                     fg=_WG_MUTED, font=get_cjk_font(9)).pack(pady=12)
            return
        for p in plugins:
            self._render_row(p)

    def _render_row(self, plugin: dict):
        bg = _WG_CARD_BG
        pid = str(plugin.get('plugin_id') or '')
        name = str(plugin.get('name') or pid)
        author = str(plugin.get('author') or '')
        installed = pid in self._installed

        row = tk.Frame(self._list_frame, bg=bg, highlightthickness=1,
                       highlightbackground=_WG_BORDER, cursor='hand2')
        row.pack(fill='x', pady=3)

        rail = tk.Frame(row, bg=_WG_GOLD if installed else _WG_SEP, width=3)
        rail.pack(side='left', fill='y')

        inner = tk.Frame(row, bg=bg)
        inner.pack(fill='x', padx=8, pady=6)

        tk.Label(inner, text=name, bg=bg, fg=_WG_TEXT,
                 font=get_cjk_font(10, True), anchor='w').pack(fill='x')
        if author:
            tk.Label(inner, text=author, bg=bg, fg=_WG_MUTED,
                     font=get_cjk_font(8), anchor='w').pack(fill='x')

        badge_row = tk.Frame(inner, bg=bg)
        badge_row.pack(fill='x', pady=(3, 0))
        if installed:
            tk.Label(badge_row, text='✓ 已安装', bg='#E8F5E9',
                     fg=_WG_GREEN, font=get_cjk_font(8),
                     padx=4, pady=1).pack(side='left')
        else:
            lbl = tk.Label(badge_row, text='安装', bg=_WG_GOLD,
                           fg='#FFFFFF', font=get_cjk_font(8, True),
                           padx=6, pady=1, cursor='hand2')
            lbl.pack(side='left')
            lbl.bind('<Button-1>', lambda e, pid=pid: self._install(pid))
            lbl.bind('<Enter>', lambda e, b=lbl: b.configure(bg=_WG_GOLD_DARK))
            lbl.bind('<Leave>', lambda e, b=lbl: b.configure(bg=_WG_GOLD))

        row.bind('<Enter>', lambda e, r=row: r.configure(highlightbackground=_WG_GOLD))
        row.bind('<Leave>', lambda e, r=row: r.configure(highlightbackground=_WG_BORDER))
        for child in (row, inner):
            child.bind('<Button-1>', lambda e, pid=pid: self._show_detail(pid))

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

    def _show_detail(self, plugin_id: str):
        self._open_full()

    def _open_full(self):
        if callable(self._on_open_full):
            self._on_open_full()


# ═══════════════════════════════════════════════════════════════════
#  WorkshopPanel — full floating panel
# ═══════════════════════════════════════════════════════════════════

class WorkshopPanel:
    """Full-featured workshop floating panel with white+gold theme."""

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
        self._installed: list[str] = []
        self._loading = False
        self._page = 1
        self._per_page = 30
        self._total = 0
        self._zoom = 1.0
        self._detail_plugin: Optional[dict] = None
        self._resize_start: Optional[tuple] = None

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
        self._load_catalog()

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

        # Header
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

        # Close button
        close_btn = tk.Label(hdr_inner, text='×', bg=_WG_HEADER_BG,
                             fg=_WG_MUTED, font=('Consolas', 16),
                             cursor='hand2', padx=6)
        close_btn.pack(side='right')
        close_btn.bind('<Button-1>', lambda e: self.hide())
        close_btn.bind('<Enter>', lambda e: close_btn.configure(fg=_WG_RED))
        close_btn.bind('<Leave>', lambda e: close_btn.configure(fg=_WG_MUTED))

        # Header border
        tk.Frame(win, bg=_WG_BORDER, height=1).pack(fill='x')

        # Toolbar
        toolbar = tk.Frame(win, bg=_WG_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(8, 4))

        # Search
        search_frame = tk.Frame(toolbar, bg=_WG_CARD_BG, highlightthickness=1,
                                highlightbackground=_WG_BORDER)
        search_frame.pack(side='left', fill='x', expand=True)
        tk.Label(search_frame, text='🔍', bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(10)).pack(side='left', padx=(6, 0))
        entry = tk.Entry(search_frame, textvariable=self._search_var,
                         bg=_WG_CARD_BG, fg=_WG_TEXT, insertbackground=_WG_GOLD,
                         relief='flat', bd=0, highlightthickness=0,
                         font=get_cjk_font(10))
        entry.pack(side='left', fill='x', expand=True, padx=4, ipady=4)
        self._search_var.trace_add('write', lambda *_: self._on_search())

        # Zoom controls
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

        # Body (scrollable)
        body = tk.Frame(win, bg=_WG_BODY_BG)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        canvas = tk.Canvas(body, bg=_WG_BODY_BG, highlightthickness=0, bd=0)
        scrollbar = tk.Scrollbar(body, orient='vertical', command=canvas.yview)
        self._grid_frame = tk.Frame(canvas, bg=_WG_BODY_BG)
        self._grid_frame.bind('<Configure>',
                              lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
        self._canvas_win = canvas.create_window((0, 0), window=self._grid_frame,
                                                 anchor='nw')
        canvas.bind('<Configure>',
                    lambda e: canvas.itemconfigure(self._canvas_win, width=e.width))
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side='left', fill='both', expand=True, padx=(14, 0), pady=(4, 0))
        scrollbar.pack(side='right', fill='y', padx=(0, 4), pady=(4, 0))
        self._canvas = canvas

        # Mouse wheel scrolling
        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1 * (event.delta / 120)), 'units')
        canvas.bind_all('<MouseWheel>', _on_mousewheel)

        # Status bar
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

        # Pagination frame
        self._page_frame = tk.Frame(status_bar, bg=_WG_HEADER_BG)
        self._page_frame.pack(side='right', padx=(0, 8))

        # Resize grip
        grip = tk.Label(win, text='⋱', bg=_WG_HEADER_BG, fg=_WG_BORDER,
                        font=('Consolas', 10), cursor='size_nw_se')
        grip.place(relx=1.0, rely=1.0, anchor='se', x=-2, y=-2)
        grip.bind('<ButtonPress-1>', self._resize_start_cb)
        grip.bind('<B1-Motion>', self._resize_motion_cb)

        win.protocol('WM_DELETE_WINDOW', self.hide)

    # ── Resize ─────────────────────────────────────────────────────

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

    # ── Zoom ───────────────────────────────────────────────────────

    def _zoom_in(self):
        self._zoom = min(1.5, self._zoom + 0.1)
        self._render_grid(self._plugins)

    def _zoom_out(self):
        self._zoom = max(0.7, self._zoom - 0.1)
        self._render_grid(self._plugins)

    # ── Search ─────────────────────────────────────────────────────

    def _on_search(self):
        self._page = 1
        self._load_catalog()

    # ── Data loading ───────────────────────────────────────────────

    def _load_catalog(self):
        if self._loading:
            return
        self._loading = True
        self._render_loading()
        self._installed = _get_installed_ids(self.owner)

        def _fetch():
            client = _get_workshop_client()
            if client is None:
                self.root.after(0, lambda: self._on_error('Workshop client unavailable'))
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
        self._plugins = list(plugins)
        self._total = total
        self._status_dot.configure(fg=_WG_GREEN)
        self._status_var.set(f'{total} plugins available')
        self._render_grid(plugins)
        self._render_pagination()

    def _on_error(self, msg: str):
        self._loading = False
        self._status_dot.configure(fg=_WG_RED)
        self._status_var.set(str(msg))
        self._render_empty(msg)

    # ── Render ─────────────────────────────────────────────────────

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
            row = idx // cols
            col = idx % cols
            self._render_card(self._grid_frame, plugin, row, col, card_w, z)

        for c in range(cols):
            self._grid_frame.columnconfigure(c, weight=1, uniform='wscard')

    def _render_card(self, parent: tk.Frame, plugin: dict,
                     row: int, col: int, card_w: int, z: float):
        pid = str(plugin.get('plugin_id') or '')
        name = str(plugin.get('name') or pid)
        author = str(plugin.get('author') or 'Unknown')
        desc = str(plugin.get('description') or '')[:80]
        installed = pid in self._installed
        downloads = _finite_int(plugin.get('download_count'), 0, lo=0)

        base_font = max(8, int(9 * z))
        title_font = max(9, int(11 * z))

        # Card frame
        card = tk.Frame(parent, bg=_WG_CARD_BG, highlightthickness=1,
                        highlightbackground=_WG_BORDER, cursor='hand2')
        card.grid(row=row, column=col, padx=_CARD_PAD, pady=_CARD_PAD,
                  sticky='nsew')

        inner = tk.Frame(card, bg=_WG_CARD_BG)
        inner.pack(fill='both', expand=True, padx=int(10 * z), pady=int(8 * z))

        # Top: icon + title
        top = tk.Frame(inner, bg=_WG_CARD_BG)
        top.pack(fill='x', pady=(0, 4))
        tk.Label(top, text='◇', bg=_WG_BADGE_BG, fg=_WG_GOLD,
                 font=get_sao_font(int(14 * z)), width=2,
                 relief='flat').pack(side='left', padx=(0, 8))
        title_area = tk.Frame(top, bg=_WG_CARD_BG)
        title_area.pack(side='left', fill='x', expand=True)
        tk.Label(title_area, text=name, bg=_WG_CARD_BG, fg=_WG_TEXT,
                 font=get_cjk_font(title_font, True), anchor='w').pack(fill='x')
        tk.Label(title_area, text=f'by {author}', bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(base_font), anchor='w').pack(fill='x')

        # Description
        tk.Label(inner, text=desc, bg=_WG_CARD_BG, fg=_WG_MUTED,
                 font=get_cjk_font(base_font), anchor='w', justify='left',
                 wraplength=max(160, card_w - 40)).pack(fill='x', pady=(2, 6))

        # Footer: badges + install
        footer = tk.Frame(inner, bg=_WG_CARD_BG)
        footer.pack(fill='x')

        # Download count badge
        tk.Label(footer, text=f'↓ {downloads}', bg=_WG_BADGE_BG,
                 fg=_WG_MUTED, font=get_cjk_font(max(7, int(8 * z))),
                 padx=4, pady=1).pack(side='left', padx=(0, 4))

        if installed:
            tk.Label(footer, text='✓ 已安装', bg='#E8F5E9',
                     fg=_WG_GREEN, font=get_cjk_font(max(7, int(8 * z))),
                     padx=6, pady=1).pack(side='right')
        else:
            install_btn = tk.Label(footer, text='安装', bg=_WG_GOLD,
                                   fg='#FFFFFF',
                                   font=get_cjk_font(max(7, int(8 * z)), True),
                                   padx=8, pady=2, cursor='hand2')
            install_btn.pack(side='right')
            install_btn.bind('<Button-1>',
                             lambda e, p=pid: self._install_plugin(p))
            install_btn.bind('<Enter>',
                             lambda e, b=install_btn: b.configure(bg=_WG_GOLD_DARK))
            install_btn.bind('<Leave>',
                             lambda e, b=install_btn: b.configure(bg=_WG_GOLD))

        # Card hover
        def _enter(e, c=card):
            c.configure(highlightbackground=_WG_GOLD)
        def _leave(e, c=card):
            c.configure(highlightbackground=_WG_BORDER)
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

    # ── Install ────────────────────────────────────────────────────

    def _install_plugin(self, plugin_id: str):
        self._status_var.set(f'Installing {plugin_id}...')

        def _do():
            try:
                client = _get_workshop_client()
                if client is None:
                    self.root.after(0, lambda: self._status_var.set('Client unavailable'))
                    return
                detail = client.detail(plugin_id)
                meta = (detail.get('plugin', {})
                        if isinstance(detail, dict) else {})
                import tempfile
                with tempfile.TemporaryDirectory() as tmp:
                    path = client.download(plugin_id, tmp,
                                           expected_sha256=meta.get('sha256', ''))
                    try:
                        from act_platform.runtime import act_plugin_import
                        act_plugin_import(self.owner, path)
                    except ImportError:
                        from act_platform.plugin_install import install_plugin_archive
                        import os
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
                self.root.after(0, lambda e=str(exc):
                               self._status_var.set(f'Install failed: {e}'))

        threading.Thread(target=_do, daemon=True).start()


__all__ = ['WorkshopPanel', 'WorkshopChildPreview']
