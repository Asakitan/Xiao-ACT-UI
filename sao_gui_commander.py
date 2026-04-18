from __future__ import annotations

import tkinter as tk
from typing import Any, Dict, List, Optional, Tuple

from sao_web_panel_common import (
    PANEL_BG,
    PANEL_BG_ALT,
    PANEL_CARD,
    PANEL_CARD_ALT,
    PANEL_EDGE,
    PANEL_HEADER_ALT,
    TEXT_DIM,
    TEXT_MAIN,
    TEXT_MUTED,
    GOLD,
    GOLD_STRONG,
    CYAN,
    READY,
    apply_surface_chrome,
    attach_tab_underline,
    bind_drag,
    calc_panel_geometry,
    clear_frame,
    create_scrollable_area,
    make_section_title,
    make_tab_label,
    panel_font,
    place_corner_accents,
    set_tab_active,
)


def _fmt_time(ms: Any) -> str:
    try:
        total_ms = float(ms or 0)
    except Exception:
        return ''
    if total_ms <= 0:
        return ''
    total_s = int(-(-total_ms // 1000))
    if total_s < 60:
        return f'{total_s}s'
    return f'{total_s // 60}:{total_s % 60:02d}'


def _fmt_fp(value: Any) -> str:
    try:
        fp = float(value or 0)
    except Exception:
        return '--'
    if fp <= 0:
        return '--'
    if fp >= 10_000:
        return f'{fp / 10_000:.1f}w'
    return str(int(fp))


def _hp_color(pct: float) -> str:
    if pct > 0.5:
        return READY
    if pct > 0.2:
        return '#f4fa49'
    return '#ef684e'


class CommanderPanel:
    def __init__(self, root: tk.Tk):
        self.root = root
        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._data: Dict[str, Any] = {}
        self._active_tab = 'team'
        self._drag_ox = 0
        self._drag_oy = 0
        self._body: Optional[tk.Frame] = None
        self._canvas: Optional[tk.Canvas] = None
        self._tab_labels: Dict[str, tk.Label] = {}
        self._last_signature: Optional[Tuple[Any, ...]] = None

    def is_visible(self) -> bool:
        return bool(self._visible and self._win and self._win.winfo_exists())

    def show(self) -> None:
        if self._win is None or not self._win.winfo_exists():
            self._build()
        self._visible = True
        try:
            self._win.deiconify()
            self._win.lift()
        except Exception:
            pass
        self._render_if_needed(force=True)

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

    def update(self, data: Dict[str, Any]) -> None:
        if not isinstance(data, dict):
            return
        self._data = data
        if self._visible:
            self._render_if_needed(force=False)

    def _build(self) -> None:
        width, height, pos_x, pos_y = calc_panel_geometry(
            self.root,
            min_w=300,
            min_h=380,
            width_ratio=0.18,
            height_ratio=0.42,
            x_ratio=0.25,
            y_ratio=0.15,
        )
        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.configure(bg=PANEL_EDGE)
        win.geometry(f'{width}x{height}+{pos_x}+{pos_y}')
        try:
            win.attributes('-topmost', True)
        except Exception:
            pass
        win.bind('<Escape>', lambda _event: self.hide())
        self._win = win

        shell = tk.Frame(win, bg=PANEL_BG, highlightthickness=1, highlightbackground=PANEL_EDGE)
        shell.pack(fill=tk.BOTH, expand=True)
        apply_surface_chrome(shell, accent=CYAN, accent_side='top')
        place_corner_accents(shell, size=42)

        tk.Frame(shell, bg=CYAN, height=1).place(x=0, y=0, relwidth=1.0)
        tk.Frame(shell, bg=GOLD_STRONG, height=1).place(relx=0.0, rely=1.0, y=-1, relwidth=1.0)
        tk.Frame(shell, bg=CYAN, width=1).place(x=0, y=0, relheight=1.0)
        tk.Frame(shell, bg=GOLD_STRONG, width=1).place(relx=1.0, x=-1, y=0, relheight=1.0)

        header = tk.Frame(shell, bg=PANEL_HEADER_ALT, height=30)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        apply_surface_chrome(header)
        tk.Label(header, text='◇', bg=PANEL_HEADER_ALT, fg=CYAN, font=panel_font(8, bold=True)).pack(side=tk.LEFT, padx=(12, 6))
        tk.Label(header, text='COMMANDER', bg=PANEL_HEADER_ALT, fg=TEXT_MAIN, font=panel_font(11, bold=True)).pack(side=tk.LEFT)
        accent = tk.Canvas(header, bg=PANEL_HEADER_ALT, highlightthickness=0, bd=0, height=2)
        accent.pack(side=tk.BOTTOM, fill=tk.X)
        accent.bind('<Configure>', lambda event: self._draw_header_accent(accent, event.width))
        bind_drag(header, self._on_drag_start, self._on_drag_move)

        tabs = tk.Frame(shell, bg=PANEL_HEADER_ALT, height=26)
        tabs.pack(fill=tk.X)
        tabs.pack_propagate(False)
        for key, label in (('team', 'TEAM'), ('boss', 'BOSS RAID')):
            slot = tk.Frame(tabs, bg=PANEL_HEADER_ALT)
            slot.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)
            tab = make_tab_label(slot, label, command=lambda target=key: self._switch_tab(target))
            tab.configure(bg=PANEL_HEADER_ALT)
            tab.pack(fill=tk.X, expand=True)
            attach_tab_underline(tab, slot)
            self._tab_labels[key] = tab
        self._refresh_tabs()

        body_wrap = tk.Frame(shell, bg=PANEL_BG, padx=10, pady=8)
        body_wrap.pack(fill=tk.BOTH, expand=True)
        _, canvas, body = create_scrollable_area(body_wrap, PANEL_BG)
        self._canvas = canvas
        self._body = body

        footer = tk.Frame(shell, bg=PANEL_HEADER_ALT, height=20)
        footer.pack(fill=tk.X)
        footer.pack_propagate(False)
        apply_surface_chrome(footer, accent=GOLD_STRONG, accent_side='bottom')
        tk.Label(footer, text='COMMANDER PANEL — LIVE', bg=PANEL_HEADER_ALT, fg=TEXT_DIM, font=panel_font(7, bold=True)).pack(expand=True)

    def _draw_header_accent(self, canvas: tk.Canvas, width: int) -> None:
        canvas.delete('all')
        if width <= 2:
            return
        half = width // 2
        canvas.create_line(0, 1, half, 1, fill=CYAN, width=2)
        canvas.create_line(half, 1, width, 1, fill=GOLD_STRONG, width=2)

    def _switch_tab(self, tab: str) -> None:
        if tab == self._active_tab:
            return
        self._active_tab = tab
        self._refresh_tabs()
        self._render_if_needed(force=True)

    def _refresh_tabs(self) -> None:
        for key, label in self._tab_labels.items():
            set_tab_active(label, key == self._active_tab)

    def _render_if_needed(self, force: bool = False) -> None:
        if self._body is None:
            return
        signature = (
            self._active_tab,
            int(self._data.get('dungeon_id') or 0),
            tuple(
                (
                    int(member.get('uid') or 0),
                    str(member.get('name') or ''),
                    str(member.get('profession') or ''),
                    int(member.get('fight_point') or 0),
                    int(member.get('level') or 0),
                    bool(member.get('is_self')),
                    bool(member.get('is_leader')),
                    int(member.get('hp') or 0),
                    int(member.get('max_hp') or 0),
                    tuple(
                        (
                            int(slot.get('index') or 0),
                            str(slot.get('state') or ''),
                            round(float(slot.get('cooldown_pct') or 0.0), 3),
                            int(slot.get('remaining_ms') or 0),
                        )
                        for slot in list(member.get('skill_slots') or [])
                    ),
                )
                for member in list(self._data.get('members') or [])
            ),
        )
        if not force and signature == self._last_signature:
            return
        self._last_signature = signature
        clear_frame(self._body)
        if self._active_tab == 'team':
            self._render_team_tab()
        else:
            self._render_boss_tab()
        try:
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        except Exception:
            pass

    def _render_team_tab(self) -> None:
        members = list(self._data.get('members') or [])
        make_section_title(self._body, 'PARTY')
        if not members:
            self._render_empty('⚔', '暂无队伍信息\nNo team data — join a party to see members')
            return
        for member in members:
            self._member_card(member, compact=False)

    def _render_boss_tab(self) -> None:
        make_section_title(self._body, 'BOSS RAID')
        dungeon_id = self._data.get('dungeon_id')
        if not dungeon_id:
            self._render_empty('⚑', '未进入副本\nNot in a dungeon instance')
        else:
            card = tk.Frame(self._body, bg=PANEL_CARD, highlightbackground=PANEL_EDGE, highlightthickness=1, padx=10, pady=8)
            card.pack(fill=tk.X, pady=(0, 6))
            apply_surface_chrome(card, accent=GOLD_STRONG)
            tk.Label(card, text=f'副本 Dungeon ID: {dungeon_id}', bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(10, bold=True)).pack(anchor='w')
            tk.Label(card, text='ACTIVE', bg=PANEL_CARD, fg=GOLD_STRONG, font=panel_font(8, bold=True)).pack(anchor='w', pady=(2, 0))

        members = list(self._data.get('members') or [])
        if members:
            make_section_title(self._body, 'TEAM OVERVIEW')
            for member in members:
                self._member_card(member, compact=True)

    def _render_empty(self, icon: str, text: str) -> None:
        wrap = tk.Frame(self._body, bg=PANEL_BG)
        wrap.pack(fill=tk.X, pady=28)
        tk.Label(wrap, text=icon, bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(22, bold=True)).pack()
        tk.Label(wrap, text=text, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(9), justify='center').pack(pady=(6, 0))

    def _member_card(self, member: Dict[str, Any], compact: bool) -> None:
        is_self = bool(member.get('is_self'))
        is_leader = bool(member.get('is_leader'))
        border = CYAN if is_self else PANEL_EDGE
        card = tk.Frame(self._body, bg=PANEL_CARD, highlightbackground=border, highlightthickness=2 if is_self else 1, padx=8, pady=6)
        card.pack(fill=tk.X, pady=(0, 5))
        apply_surface_chrome(card, accent=CYAN if is_self else GOLD_STRONG if is_leader else PANEL_BG_ALT)

        top = tk.Frame(card, bg=PANEL_CARD)
        top.pack(fill=tk.X)
        name = str(member.get('name') or f'UID:{member.get("uid") or 0}')
        tk.Label(top, text=name, bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(10 if compact else 11, bold=True)).pack(side=tk.LEFT, fill=tk.X, expand=True)
        profession = str(member.get('profession') or '')
        if profession:
            tk.Label(top, text=profession, bg=PANEL_HEADER_ALT, fg=TEXT_MAIN, font=panel_font(8), padx=5, pady=1).pack(side=tk.RIGHT)
        if is_leader:
            tk.Label(top, text='★', bg=PANEL_CARD, fg=GOLD_STRONG, font=panel_font(9, bold=True)).pack(side=tk.RIGHT, padx=(0, 4))

        if not compact:
            meta = tk.Frame(card, bg=PANEL_CARD)
            meta.pack(fill=tk.X, pady=(2, 0))
            level = member.get('level') or '--'
            tk.Label(meta, text=f'Lv.{level}', bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT)
            tk.Label(meta, text=f'CP {_fmt_fp(member.get("fight_point"))}', bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT, padx=(10, 0))
            if is_self:
                tk.Label(meta, text='SELF', bg=PANEL_CARD, fg=CYAN, font=panel_font(8, bold=True)).pack(side=tk.LEFT, padx=(8, 0))

            try:
                hp = float(member.get('hp') or 0)
                hp_max = float(member.get('max_hp') or 0)
            except Exception:
                hp = hp_max = 0.0
            if hp_max > 0:
                self._hp_mini_bar(card, max(0.0, min(1.0, hp / hp_max)))

        if is_self:
            slots = list(member.get('skill_slots') or [])
            if slots:
                self._cd_grid(card, slots)

    def _hp_mini_bar(self, parent: tk.Frame, pct: float) -> None:
        row = tk.Frame(parent, bg=PANEL_CARD)
        row.pack(fill=tk.X, pady=(4, 0))
        canvas = tk.Canvas(row, height=6, bg=PANEL_CARD_ALT, highlightthickness=0, bd=0)
        canvas.pack(side=tk.LEFT, fill=tk.X, expand=True)
        canvas.update_idletasks()
        width = max(120, int(canvas.winfo_reqwidth() or 240))
        canvas.create_rectangle(0, 0, width, 6, fill=PANEL_CARD_ALT, outline='')
        canvas.create_rectangle(0, 0, int(width * pct), 6, fill=_hp_color(pct), outline='')
        tk.Label(row, text=f'{int(round(pct * 100))}%', bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8, bold=True)).pack(side=tk.RIGHT, padx=(6, 0))

    def _cd_grid(self, parent: tk.Frame, slots: List[Dict[str, Any]]) -> None:
        grid = tk.Frame(parent, bg=PANEL_CARD)
        grid.pack(fill=tk.X, pady=(5, 0))
        for idx, slot in enumerate(slots):
            state = str(slot.get('state') or 'ready').lower()
            border = PANEL_EDGE
            if state == 'ready':
                border = READY
            elif state == 'active':
                border = GOLD_STRONG
            cell = tk.Frame(grid, bg=PANEL_CARD_ALT, highlightbackground=border, highlightthickness=1, width=32, height=32)
            cell.grid(row=0, column=idx, padx=2)
            cell.grid_propagate(False)
            canvas = tk.Canvas(cell, width=30, height=30, bg=PANEL_CARD_ALT, highlightthickness=0, bd=0)
            canvas.place(x=1, y=1)
            if state == 'cooldown':
                fill_height = int(30 * max(0.0, min(1.0, float(slot.get('cooldown_pct') or 0.0))))
                canvas.create_rectangle(0, 30 - fill_height, 30, 30, fill='#f3af1222', outline='')
            canvas.create_text(15, 9, text=str(slot.get('index') or idx + 1), fill=TEXT_DIM, font=panel_font(6, bold=True))
            time_text = _fmt_time(slot.get('remaining_ms') or 0) if state == 'cooldown' else '✓'
            canvas.create_text(15, 21, text=time_text, fill=READY if state == 'ready' else TEXT_MAIN, font=panel_font(6, bold=True))

    def _on_drag_start(self, event) -> None:
        if self._win is None:
            return
        self._drag_ox = event.x_root - self._win.winfo_x()
        self._drag_oy = event.y_root - self._win.winfo_y()

    def _on_drag_move(self, event) -> None:
        if self._win is None:
            return
        self._win.geometry(f'+{event.x_root - self._drag_ox}+{event.y_root - self._drag_oy}')