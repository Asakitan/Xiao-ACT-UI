# -*- coding: utf-8 -*-
"""
sao_gui_commander.py — SAO Commander panel (tkinter port of web/commander.html).

A lightweight party / boss dashboard. Unlike the in-game ULW overlays this
is a regular Toplevel the user opens/closes on demand, so it uses tk
widgets for the layout instead of bit-baked PIL frames.

The visual palette matches the HP overlay (olive-beige SAO cover) so it
sits consistently with the rest of the HUD.

Public API mirrors the JS surface called from `sao_gui.py`:

    CommanderPanel(root)
        .show()                  # open (idempotent; refreshes data)
        .hide()                  # close
        .destroy()
        .update(data)            # data shape from commander.html:
                                 #   {dungeon_id, members:[...]}
        .is_visible() -> bool
"""

from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont
from typing import Any, Callable, Dict, List, Optional
from window_effects import apply_native_chrome


# ═══════════════════════════════════════════════
#  Palette (from commander.html CSS custom properties)
# ═══════════════════════════════════════════════

BG_OLIVE = '#c9cab2'        # panel background
BG_HEADER = '#b7b8a2'       # title bar
CARD_BG = '#d5d6bf'
CARD_BORDER = '#a9aa94'
TEXT_MAIN = '#3c3e32'
TEXT_MUTED = '#6a6c5c'
TEXT_DIM = '#8b8c7b'
ACCENT_CYAN = '#68e4ff'
ACCENT_GOLD = '#d49c17'
HP_GREEN = '#9ad334'
HP_YELLOW = '#f4fa49'
HP_RED = '#ef684e'
SELF_HL = '#6db3c8'
LEADER_HL = '#d49c17'


def _fmt_time(ms: float) -> str:
    try:
        ms = float(ms or 0)
    except Exception:
        return ''
    if ms <= 0:
        return ''
    s = int(-(-ms // 1000))  # ceil
    if s < 60:
        return f'{s}s'
    return f'{s // 60}:{s % 60:02d}'


def _fmt_fp(fp: float) -> str:
    try:
        fp = float(fp or 0)
    except Exception:
        return '--'
    if fp <= 0:
        return '--'
    if fp >= 10000:
        return f'{fp / 10000:.1f}w'
    return str(int(fp))


def _hp_color(pct: float) -> str:
    if pct > 0.5:
        return HP_GREEN
    if pct > 0.2:
        return HP_YELLOW
    return HP_RED


# ═══════════════════════════════════════════════
#  Panel
# ═══════════════════════════════════════════════

class CommanderPanel:
    WIDTH = 420
    HEIGHT = 720

    def __init__(self, root: tk.Tk):
        self.root = root
        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._data: Dict[str, Any] = {}
        self._active_tab = 'team'          # 'team' | 'boss'
        self._drag_ox = 0
        self._drag_oy = 0
        # Cached widgets
        self._body: Optional[tk.Frame] = None
        self._tab_btns: Dict[str, tk.Label] = {}

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def is_visible(self) -> bool:
        return self._visible and self._win is not None

    def show(self) -> None:
        if self._win is not None and self._win.winfo_exists():
            self._visible = True
            try:
                self._win.deiconify()
                self._win.lift()
            except Exception:
                pass
            self._refresh()
            return
        self._build()
        self._visible = True
        self._refresh()

    def hide(self) -> None:
        if self._win is not None:
            try:
                self._win.withdraw()
            except Exception:
                pass
        self._visible = False

    def destroy(self) -> None:
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._visible = False

    # ──────────────────────────────────────────
    #  Data push
    # ──────────────────────────────────────────

    def update(self, data: Dict[str, Any]) -> None:
        if not isinstance(data, dict):
            return
        self._data = data
        if self._visible:
            self._refresh()

    # ──────────────────────────────────────────
    #  UI construction
    # ──────────────────────────────────────────

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.configure(bg=BG_OLIVE, highlightthickness=0)
        win.geometry(f'{self.WIDTH}x{self.HEIGHT}+120+120')
        apply_native_chrome(win, dark_caption=False)
        win.attributes('-topmost', True)
        self._win = win

        # ── Title bar ──
        title = tk.Frame(win, bg=BG_HEADER, height=34)
        title.pack(fill=tk.X, side=tk.TOP)
        title.pack_propagate(False)

        # diamond icon + title
        tk.Label(title, text='◆', bg=BG_HEADER, fg=ACCENT_CYAN,
                 font=self._font(14, bold=True)).pack(side=tk.LEFT, padx=(12, 6))
        tk.Label(title, text='COMMANDER', bg=BG_HEADER, fg=TEXT_MAIN,
                 font=self._font(13, bold=True))\
            .pack(side=tk.LEFT, pady=(6, 0))

        # close button
        close = tk.Label(title, text='✕', bg=BG_HEADER, fg=TEXT_MUTED,
                         font=self._font(13, bold=True), cursor='hand2')
        close.pack(side=tk.RIGHT, padx=10)
        close.bind('<Button-1>', lambda e: self.hide())
        close.bind('<Enter>', lambda e: close.config(fg=HP_RED))
        close.bind('<Leave>', lambda e: close.config(fg=TEXT_MUTED))

        # drag
        for w in (title, ):
            w.bind('<Button-1>', self._on_drag_start)
            w.bind('<B1-Motion>', self._on_drag_move)

        # ── Tab bar ──
        tabs = tk.Frame(win, bg=BG_OLIVE, height=36)
        tabs.pack(fill=tk.X, side=tk.TOP)
        tabs.pack_propagate(False)

        for key, label in (('team', 'TEAM'), ('boss', 'BOSS RAID')):
            btn = tk.Label(
                tabs, text=label, bg=BG_OLIVE, fg=TEXT_MAIN,
                font=self._font(11, bold=True), cursor='hand2',
                padx=18, pady=6,
            )
            btn.pack(side=tk.LEFT, expand=True, fill=tk.X)
            btn.bind('<Button-1>',
                     lambda e, k=key: self._switch_tab(k))
            self._tab_btns[key] = btn

        # underline indicator frame
        self._tab_underline = tk.Frame(win, bg=ACCENT_CYAN, height=2)
        self._tab_underline.pack(fill=tk.X, side=tk.TOP)

        # ── Body (scrollable) ──
        body_wrap = tk.Frame(win, bg=BG_OLIVE)
        body_wrap.pack(fill=tk.BOTH, expand=True, side=tk.TOP,
                       padx=12, pady=(10, 0))
        canvas = tk.Canvas(body_wrap, bg=BG_OLIVE,
                           highlightthickness=0, bd=0)
        vsb = tk.Scrollbar(body_wrap, orient=tk.VERTICAL,
                           command=canvas.yview)
        canvas.configure(yscrollcommand=vsb.set)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)

        self._body = tk.Frame(canvas, bg=BG_OLIVE)
        self._body_window = canvas.create_window(
            (0, 0), window=self._body, anchor='nw')

        def _on_body_cfg(e):
            canvas.configure(scrollregion=canvas.bbox('all'))
            canvas.itemconfigure(self._body_window, width=e.width)
        self._body.bind('<Configure>',
                        lambda e: canvas.configure(
                            scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>',
                    lambda e: canvas.itemconfigure(
                        self._body_window, width=e.width))
        # Mouse wheel
        canvas.bind_all('<MouseWheel>',
                        lambda e: canvas.yview_scroll(
                            int(-e.delta / 120), 'units'))

        # ── Footer ──
        footer = tk.Frame(win, bg=BG_HEADER, height=26)
        footer.pack(fill=tk.X, side=tk.BOTTOM)
        footer.pack_propagate(False)
        tk.Label(footer, text='COMMANDER PANEL — LIVE',
                 bg=BG_HEADER, fg=TEXT_DIM,
                 font=self._font(9, bold=True))\
            .pack(pady=4)

        self._refresh_tab_hl()

    # ──────────────────────────────────────────
    #  Tabs
    # ──────────────────────────────────────────

    def _switch_tab(self, key: str) -> None:
        if key == self._active_tab:
            return
        self._active_tab = key
        self._refresh_tab_hl()
        self._refresh()

    def _refresh_tab_hl(self) -> None:
        for k, btn in self._tab_btns.items():
            if k == self._active_tab:
                btn.configure(fg=ACCENT_GOLD)
            else:
                btn.configure(fg=TEXT_MAIN)

    # ──────────────────────────────────────────
    #  Refresh body
    # ──────────────────────────────────────────

    def _refresh(self) -> None:
        if self._body is None:
            return
        for w in self._body.winfo_children():
            w.destroy()

        members = (self._data or {}).get('members') or []
        if self._active_tab == 'team':
            self._render_team(members)
        else:
            self._render_boss(members)

    def _render_team(self, members: List[Dict[str, Any]]) -> None:
        self._section_header('PARTY')
        if not members:
            self._empty_state('⚔',
                              '暂无队伍信息\nNo team data — join a party')
            return
        for m in members:
            self._member_card(m, compact=False)

    def _render_boss(self, members: List[Dict[str, Any]]) -> None:
        self._section_header('BOSS RAID')
        dungeon_id = (self._data or {}).get('dungeon_id')
        if not dungeon_id:
            self._empty_state('⚑',
                              '未进入副本\nNot in a dungeon instance')
        else:
            card = tk.Frame(self._body, bg=CARD_BG,
                            highlightbackground=CARD_BORDER,
                            highlightthickness=1)
            card.pack(fill=tk.X, pady=4)
            tk.Label(card, text=f'副本 Dungeon ID: {dungeon_id}',
                     bg=CARD_BG, fg=TEXT_MAIN,
                     font=self._font(12, bold=True))\
                .pack(anchor='w', padx=10, pady=(8, 0))
            tk.Label(card, text='ACTIVE',
                     bg=CARD_BG, fg=ACCENT_GOLD,
                     font=self._font(10, bold=True))\
                .pack(anchor='w', padx=10, pady=(2, 8))
        if members:
            self._section_header('TEAM OVERVIEW')
            for m in members:
                self._member_card(m, compact=True)

    # ──────────────────────────────────────────
    #  Sub-widgets
    # ──────────────────────────────────────────

    def _section_header(self, title: str) -> None:
        row = tk.Frame(self._body, bg=BG_OLIVE, height=22)
        row.pack(fill=tk.X, pady=(8, 4))
        row.pack_propagate(False)
        tk.Label(row, text=title, bg=BG_OLIVE, fg=TEXT_MUTED,
                 font=self._font(10, bold=True))\
            .pack(side=tk.LEFT)
        line = tk.Frame(row, bg=CARD_BORDER, height=1)
        line.pack(side=tk.LEFT, fill=tk.X, expand=True,
                  padx=(10, 0), pady=10)

    def _empty_state(self, icon: str, text: str) -> None:
        wrap = tk.Frame(self._body, bg=BG_OLIVE)
        wrap.pack(fill=tk.X, pady=40)
        tk.Label(wrap, text=icon, bg=BG_OLIVE, fg=TEXT_DIM,
                 font=self._font(32, bold=True)).pack()
        tk.Label(wrap, text=text, bg=BG_OLIVE, fg=TEXT_DIM,
                 font=self._font(10), justify='center')\
            .pack(pady=(6, 0))

    def _member_card(self, m: Dict[str, Any], compact: bool = False) -> None:
        is_self = bool(m.get('is_self'))
        is_leader = bool(m.get('is_leader'))
        border_col = SELF_HL if is_self else CARD_BORDER
        card = tk.Frame(self._body, bg=CARD_BG,
                        highlightbackground=border_col,
                        highlightthickness=2 if is_self else 1)
        card.pack(fill=tk.X, pady=3)

        pad = (4, 8) if compact else (6, 10)

        # Top row: name + profession badge (+ leader star)
        top = tk.Frame(card, bg=CARD_BG)
        top.pack(fill=tk.X, padx=pad[1], pady=(pad[0], 2))
        name = m.get('name') or f"UID:{m.get('uid', '')}"
        name_lbl = tk.Label(top, text=name, bg=CARD_BG, fg=TEXT_MAIN,
                            font=self._font(
                                11 if compact else 12, bold=True))
        name_lbl.pack(side=tk.LEFT)
        prof = m.get('profession')
        if prof:
            badge = tk.Label(top, text=prof, bg=BG_HEADER, fg=TEXT_MAIN,
                             font=self._font(9, bold=False),
                             padx=6, pady=1)
            badge.pack(side=tk.RIGHT)
        if is_leader:
            tk.Label(top, text='★', bg=CARD_BG, fg=LEADER_HL,
                     font=self._font(11, bold=True))\
                .pack(side=tk.RIGHT, padx=(0, 4))

        if not compact:
            # Meta row
            meta = tk.Frame(card, bg=CARD_BG)
            meta.pack(fill=tk.X, padx=pad[1])
            try:
                lv = m.get('level') or '--'
            except Exception:
                lv = '--'
            tk.Label(meta, text=f'Lv.{lv}', bg=CARD_BG, fg=TEXT_MUTED,
                     font=self._font(10))\
                .pack(side=tk.LEFT, padx=(0, 10))
            tk.Label(meta, text=f'CP {_fmt_fp(m.get("fight_point"))}',
                     bg=CARD_BG, fg=TEXT_MUTED, font=self._font(10))\
                .pack(side=tk.LEFT)
            if is_self:
                tk.Label(meta, text='SELF', bg=CARD_BG,
                         fg=ACCENT_CYAN, font=self._font(9, bold=True))\
                    .pack(side=tk.LEFT, padx=(8, 0))

            # HP mini bar
            try:
                hp = float(m.get('hp', 0) or 0)
                mx = float(m.get('max_hp', 0) or 0)
            except Exception:
                hp = mx = 0.0
            if mx > 0:
                pct = max(0.0, min(1.0, hp / mx))
                self._hp_mini_bar(card, pct)

        # Skill CD grid (self only, server doesn't send others' CDs)
        if is_self:
            slots = m.get('skill_slots') or []
            if slots:
                self._cd_grid(card, slots)

        tk.Frame(card, bg=CARD_BG, height=pad[0]).pack()

    def _hp_mini_bar(self, parent: tk.Frame, pct: float) -> None:
        row = tk.Frame(parent, bg=CARD_BG)
        row.pack(fill=tk.X, padx=10, pady=(4, 0))
        w = 280
        h = 8
        cv = tk.Canvas(row, width=w, height=h, bg=BG_OLIVE,
                       highlightthickness=0, bd=0)
        cv.pack(side=tk.LEFT, fill=tk.X, expand=True)
        # background trough
        cv.create_rectangle(0, 0, w, h, fill=BG_OLIVE, outline='')
        # fill
        fw = max(1, int(w * pct))
        cv.create_rectangle(0, 0, fw, h,
                            fill=_hp_color(pct), outline='')
        tk.Label(row, text=f'{int(round(pct * 100))}%',
                 bg=CARD_BG, fg=TEXT_MUTED,
                 font=self._font(9, bold=True))\
            .pack(side=tk.RIGHT, padx=(6, 0))

    def _cd_grid(self, parent: tk.Frame,
                 slots: List[Dict[str, Any]]) -> None:
        grid = tk.Frame(parent, bg=CARD_BG)
        grid.pack(fill=tk.X, padx=10, pady=(6, 4))
        cols = 9
        for i, s in enumerate(slots):
            state = str(s.get('state') or 'ready').lower()
            try:
                cd_pct = float(s.get('cooldown_pct') or 0)
            except Exception:
                cd_pct = 0.0
            remaining = s.get('remaining_ms') or 0
            time_str = _fmt_time(remaining) if state == 'cooldown' else '✓'
            bg = BG_OLIVE if state == 'cooldown' else CARD_BG
            border = ACCENT_CYAN if state == 'ready' else CARD_BORDER
            cell = tk.Frame(grid, bg=bg,
                            highlightbackground=border,
                            highlightthickness=1,
                            width=30, height=36)
            cell.grid(row=0, column=i, padx=2)
            cell.grid_propagate(False)
            # Fill (drawn via Canvas to show cooldown level)
            cv = tk.Canvas(cell, width=28, height=34,
                           bg=bg, highlightthickness=0, bd=0)
            cv.place(x=1, y=1)
            if state == 'cooldown' and cd_pct > 0:
                fh = int(34 * max(0.0, min(1.0, cd_pct)))
                cv.create_rectangle(0, 34 - fh, 28, 34,
                                    fill=BG_HEADER, outline='')
            cv.create_text(14, 10, text=str(s.get('index', i + 1)),
                           fill=TEXT_MAIN,
                           font=self._font(8, bold=True))
            cv.create_text(14, 25, text=time_str,
                           fill=ACCENT_GOLD if state == 'ready'
                           else TEXT_MUTED,
                           font=self._font(8, bold=False))

    # ──────────────────────────────────────────
    #  Helpers
    # ──────────────────────────────────────────

    _FONT_CACHE: Dict[tuple, tkfont.Font] = {}

    @classmethod
    def _font(cls, size: int, bold: bool = False) -> tkfont.Font:
        key = (size, bold)
        f = cls._FONT_CACHE.get(key)
        if f is None:
            try:
                f = tkfont.Font(family='Segoe UI', size=size,
                                weight='bold' if bold else 'normal')
            except Exception:
                f = tkfont.Font(size=size,
                                weight='bold' if bold else 'normal')
            cls._FONT_CACHE[key] = f
        return f

    # ── drag ──

    def _on_drag_start(self, e) -> None:
        self._drag_ox = e.x_root - self._win.winfo_x()
        self._drag_oy = e.y_root - self._win.winfo_y()

    def _on_drag_move(self, e) -> None:
        if self._win is None:
            return
        self._win.geometry(
            f'+{e.x_root - self._drag_ox}+{e.y_root - self._drag_oy}')
