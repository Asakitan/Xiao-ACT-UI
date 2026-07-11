from __future__ import annotations

import math
import tkinter as tk
from typing import Any, Dict, List, Optional, Tuple

from gui_modules import sao_panel_ui as ui
from gui_modules.sao_panel_components import (
    _pc,
    action_button,
    bind_canvas_mousewheel,
    rounded_panel,
    sao_scrollbar,
    section_card,
    status_badge,
)
from gui_modules.sao_panel_ui import _bind_panel_drag, _sao_panel_body, _sao_panel_header
from utils.sao_sound import get_cjk_font


_ELLIPSIS_FONT_CACHE: Dict[Any, Any] = {}


def _panel_tokens() -> Dict[str, str]:
    return {
        'bg': _pc('bg', ui._SAO_PANEL_BG),
        'body': _pc('body_bg', ui._SAO_PANEL_BODY_BG),
        'card': _pc('card_bg', ui._SAO_PANEL_BODY_BG),
        'card_alt': _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG),
        'header': _pc('header_bg', ui._SAO_PANEL_HEADER_BG),
        'border': _pc('border', ui._SAO_PANEL_BORDER),
        'accent': _pc('accent', ui._SAO_PANEL_ACCENT),
        'gold': _pc('gold', ui._SAO_PANEL_GOLD),
        'label': _pc('label_fg', ui._SAO_PANEL_LABEL_FG),
        'value': _pc('value_fg', ui._SAO_PANEL_VALUE_FG),
        'ok': _pc('ok', '#3fae5a'),
        'danger': _pc('danger', '#ef684e'),
        'gold_soft': _pc('gold_soft', '#fbf2d8'),
    }


def _panel_geometry(master: tk.Misc) -> Tuple[int, int, int, int]:
    try:
        screen_w = int(master.winfo_screenwidth())
        screen_h = int(master.winfo_screenheight())
    except Exception:
        screen_w, screen_h = 1920, 1080
    return (
        max(300, int(min(screen_w, 1920) * 0.18)),
        max(380, int(min(screen_h, 1080) * 0.42)),
        max(16, int(screen_w * 0.25)),
        max(0, int(screen_h * 0.15)),
    )


def clear_frame(frame: tk.Widget) -> None:
    for child in frame.winfo_children():
        child.destroy()


def _scrollable_area(parent: tk.Widget, bg: str) -> Tuple[tk.Canvas, tk.Frame]:
    wrap = tk.Frame(parent, bg=bg)
    wrap.pack(fill=tk.BOTH, expand=True)
    canvas = tk.Canvas(wrap, bg=bg, highlightthickness=0, bd=0)
    scrollbar = sao_scrollbar(wrap, canvas.yview)
    body = tk.Frame(canvas, bg=bg)
    window_id = canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>', lambda _event: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda event: canvas.itemconfigure(window_id, width=event.width))
    canvas.configure(yscrollcommand=scrollbar.set)
    canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
    bind_canvas_mousewheel(canvas, body)
    return canvas, body


def _tk_ellipsize(text: str, font_spec: Any, max_px: int) -> str:
    # 按像素预算给 Tk Label 文本加省略号 (Tk 无原生 ellipsis)。
    if max_px <= 0 or not text:
        return text
    try:
        import tkinter.font as tkfont
        key = tuple(font_spec) if isinstance(font_spec, (list, tuple)) else str(font_spec)
        font_obj = _ELLIPSIS_FONT_CACHE.get(key)
        if font_obj is None:
            font_obj = tkfont.Font(font=font_spec)
            _ELLIPSIS_FONT_CACHE[key] = font_obj
        if font_obj.measure(text) <= max_px:
            return text
        while text and font_obj.measure(text + '…') > max_px:
            text = text[:-1]
        return (text + '…') if text else '…'
    except Exception:
        return text


def _finite_float(
    value: Any,
    default: float = 0.0,
    *,
    lo: Optional[float] = None,
    hi: Optional[float] = None,
) -> float:
    try:
        num = float(default if value is None or value == '' else value)
    except Exception:
        num = float(default or 0.0)
    if not math.isfinite(num):
        num = float(default or 0.0)
    if lo is not None:
        num = max(float(lo), num)
    if hi is not None:
        num = min(float(hi), num)
    return num


def _finite_int(
    value: Any,
    default: int = 0,
    *,
    lo: Optional[int] = None,
    hi: Optional[int] = None,
) -> int:
    num = int(_finite_float(value, float(default), lo=lo, hi=hi))
    if lo is not None:
        num = max(int(lo), num)
    if hi is not None:
        num = min(int(hi), num)
    return num


def _unit_pct(value: Any, default: float = 0.0) -> float:
    return _finite_float(value, default, lo=0.0, hi=1.0)


def _member_signature(member: Dict[str, Any]) -> Tuple[Any, ...]:
    slots = tuple(
        (
            _finite_int(slot.get('index'), 0, lo=0),
            str(slot.get('state') or ''),
            round(_unit_pct(slot.get('cooldown_pct')), 3),
            _finite_int(slot.get('remaining_ms'), 0, lo=0),
        )
        for slot in list(member.get('skill_slots') or [])
        if isinstance(slot, dict)
    )
    return (
        str(member.get('uid') or ''),
        str(member.get('name') or ''),
        str(member.get('profession') or ''),
        _finite_int(member.get('fight_point'), 0, lo=0),
        _finite_int(member.get('level'), 0, lo=0),
        bool(member.get('is_self')),
        bool(member.get('is_leader')),
        _finite_int(member.get('hp'), 0, lo=0),
        _finite_int(member.get('max_hp'), 0, lo=0),
        slots,
    )


def commander_data_signature(data: Dict[str, Any]) -> Tuple[Any, ...]:
    members = tuple(
        _member_signature(member)
        for member in list((data or {}).get('members') or [])
        if isinstance(member, dict)
    )
    return (
        str((data or {}).get('team_id') or ''),
        str((data or {}).get('leader_uid') or ''),
        _finite_int((data or {}).get('dungeon_id'), 0, lo=0),
        str((data or {}).get('self_uid') or ''),
        str((data or {}).get('status') or ''),
        members,
    )


def commander_panel_signature(active_tab: str, data: Dict[str, Any]) -> Tuple[Any, ...]:
    return (
        str(active_tab or ''),
        _finite_int((data or {}).get('dungeon_id'), 0, lo=0),
        str((data or {}).get('status') or ''),
        tuple(
            _member_signature(member)
            for member in list((data or {}).get('members') or [])
            if isinstance(member, dict)
        ),
    )


def _fmt_level(value: Any) -> str:
    level = _finite_int(value, 0, lo=0)
    return str(level) if level > 0 else '--'


def _fmt_time(ms: Any) -> str:
    total_ms = _finite_float(ms, 0.0, lo=0.0)
    if total_ms <= 0:
        return ''
    total_s = int(-(-total_ms // 1000))
    if total_s < 60:
        return f'{total_s}s'
    return f'{total_s // 60}:{total_s % 60:02d}'


def _fmt_fp(value: Any) -> str:
    fp = _finite_float(value, 0.0, lo=0.0)
    if fp <= 0:
        return '--'
    if fp >= 10_000:
        return f'{fp / 10_000:.1f}w'
    return str(int(fp))


def _hp_color(pct: float) -> str:
    tokens = _panel_tokens()
    if pct > 0.5:
        return tokens['ok']
    if pct > 0.2:
        return '#f4fa49'
    return tokens['danger']


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
        self._tab_labels: Dict[str, Any] = {}
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
        width, height, pos_x, pos_y = _panel_geometry(self.root)
        tokens = _panel_tokens()
        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.configure(bg=tokens['bg'])
        win.geometry(f'{width}x{height}+{pos_x}+{pos_y}')
        try:
            win.attributes('-topmost', True)
        except Exception:
            pass
        win.bind('<Escape>', lambda _event: self.hide())
        self._win = win

        header, close_label = _sao_panel_header(win, '◇', 'COMMANDER', self.hide)
        _bind_panel_drag(header, close_label, self._on_drag_start, self._on_drag_move)
        body = _sao_panel_body(win)

        tabs = tk.Frame(body, bg=tokens['body'], height=34)
        tabs.pack(fill=tk.X)
        tabs.pack_propagate(False)
        for key, label in (('team', 'TEAM'), ('boss', 'BOSS RAID')):
            slot = tk.Frame(tabs, bg=tokens['body'])
            slot.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)
            tab = action_button(
                slot, label, lambda target=key: self._switch_tab(target), kind='cyan',
                active=key == self._active_tab, canvas_bg=tokens['body'], padx=8, pady=3,
            )
            tab.pack(fill=tk.X, expand=True, padx=3, pady=3)
            self._tab_labels[key] = tab
        self._refresh_tabs()

        body_wrap = tk.Frame(body, bg=tokens['body'], padx=10, pady=8)
        body_wrap.pack(fill=tk.BOTH, expand=True)
        canvas, scroll_body = _scrollable_area(body_wrap, tokens['body'])
        self._canvas = canvas
        self._body = scroll_body

        footer = tk.Frame(body, bg=tokens['body'], height=28)
        footer.pack(fill=tk.X)
        footer.pack_propagate(False)
        status_badge(footer, 'COMMANDER PANEL — LIVE', kind='ok', bg=tokens['body']).pack(expand=True, pady=3)

    def _switch_tab(self, tab: str) -> None:
        if tab == self._active_tab:
            return
        self._active_tab = tab
        self._refresh_tabs()
        self._render_if_needed(force=True)

    def _refresh_tabs(self) -> None:
        for key, label in self._tab_labels.items():
            label.set_active(key == self._active_tab)

    def _render_if_needed(self, force: bool = False) -> None:
        if self._body is None:
            return
        signature = commander_panel_signature(self._active_tab, self._data)
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
        section = section_card(self._body, 'PARTY', accent='cyan')
        section.pack(fill=tk.X, pady=(0, 6))
        if not members:
            if str(self._data.get('status') or '') == 'backend_not_ready':
                self._render_empty(section, '⌛', '数据源未就绪 — 启动识别后显示队伍\nData source starting — team appears once capture is running')
            else:
                self._render_empty(section, '⚔', '暂无队伍信息\nNo team data — join a party to see members')
            return
        for member in members:
            self._member_card(section, member, compact=False)

    def _render_boss_tab(self) -> None:
        tokens = _panel_tokens()
        section = section_card(self._body, 'BOSS RAID', accent='gold')
        section.pack(fill=tk.X, pady=(0, 6))
        dungeon_id = _finite_int(self._data.get('dungeon_id'), 0, lo=0)
        if not dungeon_id:
            self._render_empty(section, '⚑', '未进入副本\nNot in a dungeon instance')
        else:
            card, inner = rounded_panel(
                section, bg=tokens['card'], border=tokens['border'], radius=8, rail=tokens['gold'],
                canvas_bg=tokens['card'], pad=10,
            )
            card.pack(fill=tk.X, pady=(0, 6))
            tk.Label(inner, text=f'副本 Dungeon ID: {dungeon_id}', bg=tokens['card'], fg=tokens['value'], font=get_cjk_font(10, True)).pack(anchor='w')
            status_badge(inner, 'ACTIVE', kind='gold', bg=tokens['card']).pack(anchor='w', pady=(4, 0))

        members = list(self._data.get('members') or [])
        if members:
            overview = section_card(self._body, 'TEAM OVERVIEW', accent='cyan')
            overview.pack(fill=tk.X, pady=(0, 6))
            for member in members:
                self._member_card(overview, member, compact=True)

    def _render_empty(self, parent: tk.Misc, icon: str, text: str) -> None:
        tokens = _panel_tokens()
        wrap = tk.Frame(parent, bg=tokens['card'])
        wrap.pack(fill=tk.X, pady=28)
        tk.Label(wrap, text=icon, bg=tokens['card'], fg=tokens['label'], font=get_cjk_font(22, True)).pack()
        tk.Label(wrap, text=text, bg=tokens['card'], fg=tokens['label'], font=get_cjk_font(9), justify='center').pack(pady=(6, 0))

    def _member_card(self, parent: tk.Misc, member: Dict[str, Any], compact: bool) -> None:
        tokens = _panel_tokens()
        is_self = bool(member.get('is_self'))
        is_leader = bool(member.get('is_leader'))
        rail = tokens['accent'] if is_self else tokens['gold'] if is_leader else None
        card, inner = rounded_panel(
            parent, bg=tokens['card'], border=tokens['accent'] if is_self else tokens['border'],
            radius=8, rail=rail, canvas_bg=tokens['card'], pad=8,
        )
        card.pack(fill=tk.X, pady=(0, 5))

        top = tk.Frame(inner, bg=tokens['card'])
        top.pack(fill=tk.X)
        name = str(member.get('name') or f'UID:{member.get("uid") or 0}')
        # 徽章先 pack(RIGHT) — pack 后包者只分剩余空间, 否则长名把
        # 职业徽章/队长星整个挤出卡片; 名字再按剩余像素预算省略号截断
        # (web 端 .member-name 同款 ellipsis, batch 241)
        profession = str(member.get('profession') or '')
        if profession:
            status_badge(top, profession, kind='cyan', bg=tokens['card']).pack(side=tk.RIGHT)
        if is_leader:
            status_badge(top, 'LEADER', kind='gold', bg=tokens['card']).pack(side=tk.RIGHT, padx=(0, 4))
        name_font = get_cjk_font(10 if compact else 11, True)
        try:
            body_w = int(self._body.winfo_width() or 0)
        except Exception:
            body_w = 0
        name = _tk_ellipsize(name, name_font, max(90, (body_w or 300) - 130))
        tk.Label(top, text=name, bg=tokens['card'], fg=tokens['value'], font=name_font).pack(side=tk.LEFT, fill=tk.X, expand=True)

        if not compact:
            meta = tk.Frame(inner, bg=tokens['card'])
            meta.pack(fill=tk.X, pady=(2, 0))
            tk.Label(meta, text=f'Lv.{_fmt_level(member.get("level"))}', bg=tokens['card'], fg=tokens['label'], font=get_cjk_font(8)).pack(side=tk.LEFT)
            tk.Label(meta, text=f'CP {_fmt_fp(member.get("fight_point"))}', bg=tokens['card'], fg=tokens['label'], font=get_cjk_font(8)).pack(side=tk.LEFT, padx=(10, 0))
            if is_self:
                status_badge(meta, 'SELF', kind='cyan', bg=tokens['card']).pack(side=tk.LEFT, padx=(8, 0))

            hp = _finite_float(member.get('hp'), 0.0, lo=0.0)
            hp_max = _finite_float(member.get('max_hp'), 0.0, lo=0.0)
            if hp_max > 0:
                self._hp_mini_bar(inner, _unit_pct(hp / hp_max))

        if is_self:
            slots = list(member.get('skill_slots') or [])
            if slots:
                self._cd_grid(inner, slots)

    def _hp_mini_bar(self, parent: tk.Frame, pct: float) -> None:
        tokens = _panel_tokens()
        pct = _unit_pct(pct)
        row = tk.Frame(parent, bg=tokens['card'])
        row.pack(fill=tk.X, pady=(4, 0))
        canvas = tk.Canvas(row, height=6, bg=tokens['card_alt'], highlightthickness=0, bd=0)
        canvas.pack(side=tk.LEFT, fill=tk.X, expand=True)

        def _draw_bar(_event=None) -> None:
            current = _panel_tokens()
            width = max(120, int(canvas.winfo_width() or canvas.winfo_reqwidth() or 240))
            canvas.configure(bg=current['card_alt'])
            canvas.delete('all')
            canvas.create_rectangle(0, 0, width, 6, fill=current['card_alt'], outline='')
            canvas.create_rectangle(0, 0, int(width * pct), 6, fill=_hp_color(pct), outline='')

        canvas.bind('<Configure>', _draw_bar)
        canvas._sao_theme_repaint = _draw_bar
        _draw_bar()
        tk.Label(row, text=f'{int(round(pct * 100))}%', bg=tokens['card'], fg=tokens['label'], font=get_cjk_font(8, True)).pack(side=tk.RIGHT, padx=(6, 0))

    def _cd_grid(self, parent: tk.Frame, slots: List[Dict[str, Any]]) -> None:
        tokens = _panel_tokens()
        grid = tk.Frame(parent, bg=tokens['card'])
        grid.pack(fill=tk.X, pady=(5, 0))
        for idx, slot in enumerate(slots):
            state = str(slot.get('state') or 'ready').lower()
            border = tokens['border']
            if state == 'ready':
                border = tokens['ok']
            elif state == 'active':
                border = tokens['gold']
            cell = tk.Frame(grid, bg=tokens['card_alt'], highlightbackground=border, highlightthickness=1, width=32, height=32)
            cell.grid(row=0, column=idx, padx=2)
            cell.grid_propagate(False)
            canvas = tk.Canvas(cell, width=30, height=30, bg=tokens['card_alt'], highlightthickness=0, bd=0)
            canvas.place(x=1, y=1)
            slot_index = _finite_int(slot.get('index'), idx + 1, lo=0) or (idx + 1)
            time_text = _fmt_time(slot.get('remaining_ms') or 0) if state == 'cooldown' else '✓'

            def _draw_cell(_event=None, *, canvas=canvas, state=state, slot=slot,
                           slot_index=slot_index, time_text=time_text) -> None:
                current = _panel_tokens()
                canvas.configure(bg=current['card_alt'])
                canvas.delete('all')
                if state == 'cooldown':
                    fill_height = int(30 * _unit_pct(slot.get('cooldown_pct')))
                    canvas.create_rectangle(0, 30 - fill_height, 30, 30, fill=current['gold_soft'], outline='')
                canvas.create_text(15, 9, text=str(slot_index), fill=current['label'], font=get_cjk_font(6, True))
                canvas.create_text(15, 21, text=time_text,
                                   fill=current['ok'] if state == 'ready' else current['value'],
                                   font=get_cjk_font(6, True))

            canvas._sao_theme_repaint = _draw_cell
            _draw_cell()

    def _on_drag_start(self, event) -> None:
        if self._win is None:
            return
        self._drag_ox = event.x_root - self._win.winfo_x()
        self._drag_oy = event.y_root - self._win.winfo_y()

    def _on_drag_move(self, event) -> None:
        if self._win is None:
            return
        self._win.geometry(f'+{event.x_root - self._drag_ox}+{event.y_root - self._drag_oy}')
