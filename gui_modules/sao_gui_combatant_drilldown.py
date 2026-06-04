# -*- coding: utf-8 -*-
"""Entity-mode ACT combatant drilldown panel."""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_combatant_drilldown_back,
    act_combatant_drilldown_filter,
    act_combatant_drilldown_focus_target,
    act_combatant_drilldown_status,
)
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
)


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
        self._last_sig = ""

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
        if self._last_status and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_combatant_drilldown_status(
                self.owner,
                combatant_id=self._combatant_var.get(),
                query=self._query_var.get(),
                focus_target=self._focus_var.get(),
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "summary": {}, "skills": [], "incoming": [], "outgoing": [], "filters": {"query": self._query_var.get(), "focus_target": self._focus_var.get()}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
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
        header = _sao_panel_header(win, 'ACT COMBATANT DRILLDOWN', on_close=self.hide)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
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
        tk.Button(control, text='Focus', command=self.focus_target).pack(side='left')

        tk.Label(body, textvariable=self._status_var, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(fill='x', padx=12, pady=(0, 6))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=12, pady=(0, 12))
        canvas = tk.Canvas(outer, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = tk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        self._rows = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        skills = list(status.get('skills') or [])
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cid = str(status.get('combatant_id') or self._combatant_var.get() or '')
        if cid and self._combatant_var.get() != cid:
            self._combatant_var.set(cid)
        self._summary_var.set(f"{summary.get('name') or cid or 'NONE'} · {len(skills)} SKILLS · {self._fmt(summary.get('damage'))} DMG")
        self._status_var.set(f"encounter={status.get('encounter_id') or 'live'} · query={filters.get('query') or '-'} · focus={filters.get('focus_target') or '-'} · errors={len(status.get('errors') or [])}")
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
        max_amount = max(1, *[int(skill.get('amount') or 0) for skill in skills])
        for skill in skills[:40]:
            row = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            row.pack(fill='x', pady=2, padx=4)
            color = '#e8fff4' if skill.get('kind') == 'heal' else _SAO_PANEL_BODY_BG
            bar_width = max(4, min(160, int(160 * int(skill.get('amount') or 0) / max_amount)))
            tk.Frame(row, bg=('#2ebf86' if skill.get('kind') == 'heal' else _SAO_PANEL_GOLD), width=bar_width, height=3).pack(fill='x', anchor='w')
            values = (
                (str(skill.get('name') or '-'), 34, _SAO_PANEL_VALUE_FG),
                (str(skill.get('kind') or '-'), 10, _SAO_PANEL_LABEL_FG),
                (self._fmt(skill.get('amount')), 14, _SAO_PANEL_VALUE_FG),
                (str(skill.get('hits') or 0), 8, _SAO_PANEL_LABEL_FG),
                (self._pct(skill.get('crit_rate')), 8, _SAO_PANEL_LABEL_FG),
            )
            line = tk.Frame(row, bg=color)
            line.pack(fill='x')
            for text, width, fg in values:
                tk.Label(line, text=text, width=width, anchor='w', bg=color, fg=fg, font=('Segoe UI', 9)).pack(side='left', padx=3, pady=5)

    def _render_side(self, status: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        rows = []
        if filters.get('focus_target'):
            rows.append(('focus', filters.get('focus_target'), ''))
        for item in list(status.get('outgoing') or [])[:8]:
            if isinstance(item, Mapping):
                rows.append((item.get('kind') or 'out', item.get('name') or '-', self._fmt(item.get('amount'))))
        if not rows:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', padx=4, pady=(8, 0))
        tk.Label(box, text='OUTGOING / FOCUS', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold'), anchor='w').pack(fill='x')
        for kind, name, amount in rows:
            tk.Label(box, text=f'{kind}: {name} {amount}', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9), anchor='w').pack(fill='x', padx=8, pady=3)

    @staticmethod
    def _fmt(value: Any) -> str:
        try:
            number = float(value or 0)
        except Exception:
            return str(value or '0')
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        return str(int(number)) if number == int(number) else f"{number:.2f}"

    @staticmethod
    def _pct(value: Any) -> str:
        try:
            return f"{float(value or 0) * 100:.1f}%"
        except Exception:
            return '0.0%'

    @staticmethod
    def _signature(status: Mapping[str, Any]) -> str:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        skills = []
        for skill in list(status.get('skills') or [])[:40]:
            if isinstance(skill, Mapping):
                skills.append((skill.get('skill_id'), skill.get('name'), skill.get('amount'), skill.get('hits'), skill.get('kind')))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        return repr((status.get('combatant_id'), summary.get('damage'), summary.get('heal'), filters.get('query'), filters.get('focus_target'), skills))
