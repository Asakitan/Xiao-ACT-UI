# -*- coding: utf-8 -*-
"""Entity-mode ACT combatant drilldown panel."""

from __future__ import annotations

import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_combatant_drilldown_back,
    act_combatant_drilldown_filter,
    act_combatant_drilldown_focus_target,
    act_combatant_drilldown_status,
)
from gui_modules.sao_panel_components import more_indicator
from gui_modules.sao_panel_ui import (
    _SAO_PANEL_ACCENT,
    _SAO_PANEL_BG,
    _SAO_PANEL_BODY_BG,
    _SAO_PANEL_BORDER,
    _SAO_PANEL_GOLD,
    _SAO_PANEL_HEADER_BG,
    _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
    _theme_color,
)


def _finite_float(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> float:
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


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    num = int(_finite_float(value, float(default), lo=lo, hi=hi))
    if lo is not None:
        num = max(int(lo), num)
    if hi is not None:
        num = min(int(hi), num)
    return num


def _mapping_items(value: Any) -> list[Mapping[str, Any]]:
    if not isinstance(value, (list, tuple)):
        return []
    return [item for item in value if isinstance(item, Mapping)]


def _list_count(value: Any) -> int:
    return len(value) if isinstance(value, (list, tuple)) else 0


class CombatantDrilldownPanel:
    """SAO-styled combatant detail panel for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="COMBATANT: --")
        self._status_var = tk.StringVar(value="Ready")
        self._combatant_var = tk.StringVar(value="")
        self._query_var = tk.StringVar(value="")
        self._focus_var = tk.StringVar(value="")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[Any, ...] = ()
        self._last_sig = ""

    def select(self, combatant_id: Any = "", *, query: str = "") -> Dict[str, Any]:
        if combatant_id is not None:
            self._combatant_var.set(str(combatant_id or ""))
        if query:
            self._query_var.set(str(query or ""))
        self._last_refresh_at = 0.0
        self._last_request_key = ()
        self._last_sig = ""
        self.show()
        return self.refresh()

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
        self.refresh()

    def hide(self) -> None:
        if self._win is None:
            return
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
        self._rows = None

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        now = time.time()
        combatant_id = self._combatant_var.get()
        query = self._query_var.get()
        focus_target = self._focus_var.get()
        request_key = (combatant_id, query, focus_target)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_combatant_drilldown_status(
                self.owner,
                combatant_id=combatant_id,
                query=query,
                focus_target=focus_target,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "summary": {}, "skills": [], "incoming": [], "outgoing": [], "filters": {"query": query, "focus_target": focus_target}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def filter(self) -> Dict[str, Any]:
        return self._apply_result(act_combatant_drilldown_filter(self.owner, combatant_id=self._combatant_var.get(), query=self._query_var.get()), 'FILTER APPLIED')

    def focus_target(self) -> Dict[str, Any]:
        return self._apply_result(act_combatant_drilldown_focus_target(self.owner, combatant_id=self._combatant_var.get(), target_id=self._focus_var.get()), 'TARGET FOCUSED')

    def back(self) -> Dict[str, Any]:
        self._combatant_var.set('')
        return self._apply_result(act_combatant_drilldown_back(self.owner), 'BACK')

    def _apply_result(self, result: Mapping[str, Any], message: str) -> Dict[str, Any]:
        self._last_status = dict(result or {})
        self._last_refresh_at = time.time()
        self._last_request_key = ()
        self._status_var.set(message)
        self._render_status(self._last_status)
        return self._last_status

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Combatant Drilldown')
        win.geometry('840x540+280+155')
        win.minsize(680, 420)
        win.configure(bg=_SAO_PANEL_BG)
        try:
            win.overrideredirect(True)
            win.attributes('-alpha', 0.97)
        except Exception:
            pass
        try:
            _apply_window_icon(win)
        except Exception:
            pass
        header = _sao_panel_header(win, 'ACT COMBATANT DRILLDOWN', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'DRILLDOWN').pack(side='left')
        tk.Label(toolbar, textvariable=self._summary_var, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 10, 'bold')).pack(side='left', padx=(12, 0))
        for label, cmd in (('打开 Open', self.refresh), ('过滤 Filter', self.filter), ('返回 Back', self.back), ('关闭 Close', self.hide)):
            tk.Button(toolbar, text=label, command=cmd, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_HEADER_FG, activebackground=_SAO_PANEL_ACCENT, activeforeground='white', relief='flat', bd=0, padx=10, pady=4).pack(side='right', padx=(6, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='UID', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._combatant_var, width=14).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Search', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=18).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Target', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._focus_var, width=16).pack(side='left', padx=(6, 8))
        tk.Button(control, text='Focus', command=self.focus_target, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_HEADER_FG, activebackground=_SAO_PANEL_ACCENT, activeforeground='white', relief='flat', bd=0, padx=10, pady=2).pack(side='left')

        tk.Label(body, textvariable=self._status_var, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(fill='x', padx=12, pady=(0, 6))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=12, pady=(0, 12))
        canvas = tk.Canvas(outer, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = tk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        self._rows = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        skills = _mapping_items(status.get('skills'))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cid = str(status.get('combatant_id') or self._combatant_var.get() or '')
        if cid and self._combatant_var.get() != cid:
            self._combatant_var.set(cid)
        self._summary_var.set(f"{summary.get('name') or cid or 'NONE'} · {len(skills)} SKILLS · {self._fmt(summary.get('damage'))} DMG")
        self._status_var.set(f"encounter={status.get('encounter_id') or 'live'} · query={filters.get('query') or '-'} · focus={filters.get('focus_target') or '-'} · errors={_list_count(status.get('errors'))}")
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        for child in list(self._rows.winfo_children()):
            child.destroy()
        if not summary:
            self._render_empty()
            return
        self._render_summary(summary)
        self._render_skills(skills)
        self._render_side(status)

    def _render_empty(self) -> None:
        if self._rows is None:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(box, text='请输入 combatant UID\n可从 DPS 面板或 ACT 数据中选择角色。', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, justify='center', font=('Segoe UI', 10), pady=36).pack(fill='x')

    def _render_summary(self, summary: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        items = (
            ('Name', summary.get('name') or '-'),
            ('Damage', self._fmt(summary.get('damage'))),
            ('DPS', self._fmt(summary.get('dps'))),
            ('Heal', self._fmt(summary.get('heal'))),
            ('Crit', self._pct(summary.get('crit_rate'))),
            ('Share', self._pct(summary.get('damage_pct'))),
        )
        for label, value in items:
            card = tk.Frame(grid, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            card.pack(side='left', fill='x', expand=True, padx=2)
            tk.Label(card, text=label, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8)).pack(anchor='w', padx=6, pady=(5, 0))
            tk.Label(card, text=str(value), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Segoe UI', 10, 'bold')).pack(anchor='w', padx=6, pady=(1, 5))

    def _render_skills(self, skills: list[Mapping[str, Any]]) -> None:
        if self._rows is None:
            return
        header = tk.Frame(self._rows, bg=_SAO_PANEL_HEADER_BG)
        header.pack(fill='x', pady=(0, 2), padx=4)
        for text, width in (('Skill', 34), ('Kind', 10), ('Amount', 14), ('Hits', 8), ('Crit', 8)):
            tk.Label(header, text=text, width=width, anchor='w', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold')).pack(side='left', padx=3, pady=5)
        max_amount = max(1, *[_finite_int(skill.get('amount'), 0, lo=0) for skill in skills])
        for idx, skill in enumerate(skills[:40]):
            amount = _finite_int(skill.get('amount'), 0, lo=0)
            hits = _finite_int(skill.get('hits'), 0, lo=0)
            is_heal = skill.get('kind') == 'heal'
            if is_heal:
                color = _theme_color('ok_soft', '#e6f6ea')
            elif idx % 2:
                color = _theme_color('card_bg_alt', _SAO_PANEL_HEADER_BG)
            else:
                color = _SAO_PANEL_BODY_BG
            row = tk.Frame(self._rows, bg=color, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            row.pack(fill='x', pady=2, padx=4)
            row.configure(cursor='hand2')
            bar_width = max(4, min(160, int(160 * amount / max_amount)))
            tk.Frame(row, bg=(_theme_color('ok', '#2ebf86') if is_heal else _SAO_PANEL_GOLD), width=bar_width, height=3).pack(fill='x', anchor='w')
            values = (
                (str(skill.get('name') or '-'), 34, _SAO_PANEL_VALUE_FG),
                (str(skill.get('kind') or '-'), 10, _SAO_PANEL_LABEL_FG),
                (self._fmt(amount), 14, _SAO_PANEL_VALUE_FG),
                (str(hits), 8, _SAO_PANEL_LABEL_FG),
                (self._pct(skill.get('crit_rate')), 8, _SAO_PANEL_LABEL_FG),
            )
            line = tk.Frame(row, bg=color)
            line.pack(fill='x')
            for text, width, fg in values:
                label = tk.Label(line, text=text, width=width, anchor='w', bg=color, fg=fg, font=('Segoe UI', 9), cursor='hand2')
                label.pack(side='left', padx=3, pady=5)
                label.bind('<Button-1>', lambda _e, sk=skill: self._open_skill(sk))
            row.bind('<Button-1>', lambda _e, sk=skill: self._open_skill(sk))
        hidden = len(skills) - 40
        if hidden > 0:
            more_indicator(self._rows, hidden, noun='个技能').pack(fill='x', padx=4, pady=(2, 0))

    def _open_skill(self, skill: Mapping[str, Any]) -> None:
        sid = str(skill.get('skill_id') or skill.get('base_skill_id') or skill.get('source_skill_id') or '')
        cid = str(self._combatant_var.get() or self._last_status.get('combatant_id') or '')
        if not sid or not cid:
            self._status_var.set('SKILL DRILLDOWN NEEDS UID + SKILL ID')
            return
        panel = getattr(self.owner, '_act_skill_drilldown_panel', None)
        if panel is None:
            try:
                from gui_modules.sao_gui_skill_drilldown import SkillDrilldownPanel
                panel = SkillDrilldownPanel(self.root, self.owner)
                setattr(self.owner, '_act_skill_drilldown_panel', panel)
            except Exception as exc:
                self._status_var.set(f'SKILL PANEL ERROR: {exc}')
                return
        select = getattr(panel, 'select', None)
        if callable(select):
            select(cid, sid)
        else:
            try:
                panel.show()
            except Exception:
                pass
        self._status_var.set(f'OPEN SKILL {sid}')

    def _render_side(self, status: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        rows = []
        if filters.get('focus_target'):
            rows.append(('focus', filters.get('focus_target'), ''))
        for item in _mapping_items(status.get('outgoing'))[:8]:
            rows.append((item.get('kind') or 'out', item.get('name') or '-', self._fmt(item.get('amount'))))
        for item in _mapping_items(status.get('incoming'))[:5]:
            rows.append((
                item.get('kind') or 'in',
                item.get('name') or item.get('topic') or '-',
                self._fmt(item.get('amount') if item.get('amount') is not None else item.get('value')),
            ))
        if not rows:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', padx=4, pady=(8, 0))
        tk.Label(box, text='OUTGOING / INCOMING / FOCUS', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold'), anchor='w').pack(fill='x')
        for kind, name, amount in rows:
            tk.Label(box, text=f'{kind}: {name} {amount}', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9), anchor='w').pack(fill='x', padx=8, pady=3)

    @staticmethod
    def _fmt(value: Any) -> str:
        number = _finite_float(value, 0.0)
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        return str(int(number)) if number == int(number) else f"{number:.2f}"

    @staticmethod
    def _pct(value: Any) -> str:
        return f"{_finite_float(value, 0.0, lo=0.0, hi=1.0) * 100:.1f}%"

    @staticmethod
    def _signature(status: Mapping[str, Any]) -> str:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        skills = []
        for skill in _mapping_items(status.get('skills'))[:40]:
            skills.append((
                skill.get('skill_id'),
                skill.get('base_skill_id'),
                skill.get('source_skill_id'),
                skill.get('name'),
                skill.get('amount'),
                skill.get('hits'),
                skill.get('kind'),
                skill.get('crit_rate'),
            ))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        outgoing = []
        for item in _mapping_items(status.get('outgoing'))[:8]:
            outgoing.append((item.get('kind'), item.get('name'), item.get('amount')))
        incoming = []
        for item in _mapping_items(status.get('incoming'))[:5]:
            incoming.append((item.get('kind'), item.get('name'), item.get('topic'), item.get('amount'), item.get('value')))
        return repr((
            status.get('combatant_id'),
            summary.get('name'),
            summary.get('damage'),
            summary.get('heal'),
            summary.get('dps'),
            summary.get('hps'),
            summary.get('crit_rate'),
            summary.get('damage_pct'),
            filters.get('query'),
            filters.get('focus_target'),
            skills,
            outgoing,
            incoming,
        ))
