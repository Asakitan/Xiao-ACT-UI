# -*- coding: utf-8 -*-
"""Entity-mode ACT skill drilldown panel."""

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_skill_drilldown_back,
    act_skill_drilldown_copy,
    act_skill_drilldown_filter,
    act_skill_drilldown_status,
)
from gui_modules.sao_panel_components import fmt_clock, keep_canvas_scroll, more_indicator
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


class SkillDrilldownPanel:
    """SAO-styled per-skill detail panel for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="SKILL: --")
        self._status_var = tk.StringVar(value="Ready")
        self._combatant_var = tk.StringVar(value="")
        self._skill_var = tk.StringVar(value="")
        self._query_var = tk.StringVar(value="")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[Any, ...] = ()
        self._last_sig = ""
        self._expanded_refs: set[str] = set()

    def select(self, combatant_id: Any = "", skill_id: Any = "", *, query: str = "") -> Dict[str, Any]:
        if combatant_id is not None:
            self._combatant_var.set(str(combatant_id or ""))
        if skill_id is not None:
            self._skill_var.set(str(skill_id or ""))
        if query:
            self._query_var.set(str(query or ""))
        self._last_refresh_at = 0.0
        self._last_request_key = ()
        self._last_sig = ""
        self._expanded_refs.clear()
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
        skill_id = self._skill_var.get()
        query = self._query_var.get()
        request_key = (combatant_id, skill_id, query)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_skill_drilldown_status(
                self.owner,
                combatant_id=combatant_id,
                skill_id=skill_id,
                query=query,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "summary": {}, "timeline_refs": [], "casts": 0, "hits": 0, "crit_rate": 0.0, "filters": {"query": query}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def filter(self) -> Dict[str, Any]:
        return self._apply_result(act_skill_drilldown_filter(self.owner, combatant_id=self._combatant_var.get(), skill_id=self._skill_var.get(), query=self._query_var.get()), 'FILTER APPLIED')

    def copy(self) -> Dict[str, Any]:
        result = self._apply_result(act_skill_drilldown_copy(self.owner, combatant_id=self._combatant_var.get(), skill_id=self._skill_var.get(), query=self._query_var.get()), 'COPY READY')
        text = str(result.get('text') or '')
        if text:
            try:
                self.root.clipboard_clear()
                self.root.clipboard_append(text)
            except Exception:
                self._status_var.set('Copy payload ready, but clipboard copy failed')
        return result

    def back(self) -> Dict[str, Any]:
        self._combatant_var.set('')
        self._skill_var.set('')
        self._expanded_refs.clear()
        return self._apply_result(act_skill_drilldown_back(self.owner), 'BACK')

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
        win.title('SAO ACT Skill Drilldown')
        win.geometry('820x520+310+180')
        win.minsize(660, 400)
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
        header = _sao_panel_header(win, 'ACT SKILL DRILLDOWN', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'SKILL').pack(side='left')
        for label, cmd in (('打开 Open', self.refresh), ('过滤 Filter', self.filter), ('复制 Copy', self.copy), ('返回 Back', self.back), ('关闭 Close', self.hide)):
            tk.Button(toolbar, text=label, command=cmd, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_HEADER_FG, activebackground=_SAO_PANEL_ACCENT, activeforeground='white', relief='flat', bd=0, padx=10, pady=4).pack(side='right', padx=(6, 0))
        # summary 含未截断技能名 — 按钮先 pack 防被长名挤出窗口
        tk.Label(toolbar, textvariable=self._summary_var, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 10, 'bold')).pack(side='left', padx=(12, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='UID', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._combatant_var, width=13).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Skill', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._skill_var, width=13).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Search', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=22).pack(side='left', padx=(6, 8))

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
        self._canvas = canvas
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        refs = _mapping_items(status.get('timeline_refs'))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cid = str(status.get('combatant_id') or self._combatant_var.get() or '')
        sid = str(status.get('skill_id') or self._skill_var.get() or '')
        if cid and self._combatant_var.get() != cid:
            self._combatant_var.set(cid)
        if sid and self._skill_var.get() != sid:
            self._skill_var.set(sid)
        casts = _finite_int(status.get('casts'), 0, lo=0)
        hits = _finite_int(status.get('hits'), 0, lo=0)
        self._summary_var.set(f"{summary.get('name') or sid or 'NONE'} · {casts} CASTS · {hits} HITS")
        self._status_var.set(f"encounter={status.get('encounter_id') or 'live'} · query={filters.get('query') or '-'} · refs={len(refs)} · errors={_list_count(status.get('errors'))}")
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()
        if not summary:
            self._render_empty()
            return
        self._render_summary(status, summary)
        self._render_timeline(refs)
        self._render_payload(status)

    def _render_empty(self) -> None:
        if self._rows is None:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(box, text='请输入 combatant UID 与 skill ID\n可从成员钻取或 DPS 技能行中选择。', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, justify='center', font=('Segoe UI', 10), pady=36).pack(fill='x')

    def _render_summary(self, status: Mapping[str, Any], summary: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        items = (
            ('Skill', summary.get('name') or '-'),
            ('Kind', summary.get('kind') or '-'),
            ('Amount', self._fmt(summary.get('amount'))),
            ('Casts', self._fmt(status.get('casts'))),
            ('Hits', self._fmt(status.get('hits'))),
            ('Crit', self._pct(status.get('crit_rate'))),
        )
        for label, value in items:
            card = tk.Frame(grid, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            card.pack(side='left', fill='x', expand=True, padx=2)
            tk.Label(card, text=label, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8)).pack(anchor='w', padx=6, pady=(5, 0))
            tk.Label(card, text=str(value), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Segoe UI', 10, 'bold')).pack(anchor='w', padx=6, pady=(1, 5))

    def _render_timeline(self, refs: list[Mapping[str, Any]]) -> None:
        if self._rows is None:
            return
        header = tk.Frame(self._rows, bg=_SAO_PANEL_HEADER_BG)
        header.pack(fill='x', pady=(0, 2), padx=4)
        for text, width in (('Time', 12), ('Topic', 12), ('Label', 34), ('Value', 14)):
            tk.Label(header, text=text, width=width, anchor='w', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold')).pack(side='left', padx=3, pady=5)
        if not refs:
            tk.Label(self._rows, text='No timeline refs', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 10), pady=20).pack(fill='x')
            return
        for idx, ref in enumerate(refs[:80]):
            ref_id = str(ref.get('id') or f"ref:{idx}:{ref.get('time_ms')}")
            expanded = ref_id in self._expanded_refs
            row_bg = _theme_color('card_bg_alt', _SAO_PANEL_HEADER_BG) if idx % 2 else _SAO_PANEL_BODY_BG
            row = tk.Frame(self._rows, bg=row_bg, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            row.pack(fill='x', pady=2, padx=4)
            row.configure(cursor='hand2')
            caret = tk.Label(row, text=('▾' if expanded else '▸'), bg=row_bg, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9), cursor='hand2')
            caret.pack(side='left', padx=(4, 0), pady=5)
            caret.bind('<Button-1>', lambda _e, rid=ref_id: self._toggle_ref(rid))
            values = (
                (fmt_clock(ref.get('time_ms')), 12, _SAO_PANEL_GOLD),
                (str(ref.get('topic') or '-'), 12, _SAO_PANEL_LABEL_FG),
                (str(ref.get('label') or '-'), 34, _SAO_PANEL_VALUE_FG),
                (self._fmt(ref.get('value')), 14, _SAO_PANEL_VALUE_FG),
            )
            for text, width, fg in values:
                label = tk.Label(row, text=text, width=width, anchor='w', bg=row_bg, fg=fg, font=('Segoe UI', 9), cursor='hand2')
                label.pack(side='left', padx=3, pady=5)
                label.bind('<Button-1>', lambda _e, rid=ref_id: self._toggle_ref(rid))
            row.bind('<Button-1>', lambda _e, rid=ref_id: self._toggle_ref(rid))
            if expanded:
                self._render_ref_payload(ref)
        hidden = len(refs) - 80
        if hidden > 0:
            more_indicator(self._rows, hidden).pack(fill='x', padx=4, pady=(2, 0))

    def _toggle_ref(self, ref_id: str) -> None:
        if ref_id in self._expanded_refs:
            self._expanded_refs.remove(ref_id)
        else:
            self._expanded_refs.add(ref_id)
        self._last_sig = ""
        self._render_status(self._last_status)

    def _render_ref_payload(self, ref: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        payload = ref.get('payload') if isinstance(ref.get('payload'), Mapping) else ref
        box = tk.Frame(self._rows, bg='#0f1720', highlightthickness=1, highlightbackground=_SAO_PANEL_GOLD)
        box.pack(fill='x', padx=12, pady=(0, 4))
        text = json.dumps(payload, ensure_ascii=False, indent=2, default=str)
        tk.Label(box, text=text, bg='#0f1720', fg='#d7f7ff', font=('Consolas', 9), anchor='w', justify='left', wraplength=760).pack(fill='x', padx=8, pady=6)

    def _render_payload(self, status: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', padx=4, pady=(8, 0))
        tk.Label(box, text='技能事实 / SKILL FACTS', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold'), anchor='w').pack(fill='x')
        # Clean labeled key/value grid instead of a raw JSON blob (the previous
        # "COPY PAYLOAD PREVIEW" dump was the unreadable part). The Copy button
        # still copies the full payload.
        facts = (
            ('Combatant', status.get('combatant_id') or '-'),
            ('Skill ID', status.get('skill_id') or '-'),
            ('Damage', self._fmt(summary.get('damage'))),
            ('Heal', self._fmt(summary.get('heal'))),
            ('Casts', _finite_int(status.get('casts'), 0, lo=0)),
            ('Hits', _finite_int(status.get('hits'), 0, lo=0)),
            ('Crit', self._pct(status.get('crit_rate'))),
            ('Refs', len(_mapping_items(status.get('timeline_refs')))),
        )
        grid = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=8, pady=6)
        for idx in range(4):
            grid.grid_columnconfigure(idx, weight=1, uniform='facts')
        for i, (key, value) in enumerate(facts):
            cell = tk.Frame(grid, bg=_SAO_PANEL_BODY_BG)
            cell.grid(row=i // 4, column=i % 4, sticky='ew', padx=(0, 14), pady=3)
            tk.Label(cell, text=key.upper(), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8), anchor='w').pack(fill='x')
            tk.Label(cell, text=str(value), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Segoe UI', 11, 'bold'), anchor='w').pack(fill='x')

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

    def _signature(self, status: Mapping[str, Any]) -> str:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        refs = []
        for ref in _mapping_items(status.get('timeline_refs'))[:80]:
            refs.append((ref.get('id'), ref.get('time_ms'), ref.get('topic'), ref.get('label'), ref.get('value'), ref.get('payload') if str(ref.get('id')) in self._expanded_refs else None))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        return repr((
            status.get('combatant_id'),
            status.get('skill_id'),
            summary.get('name'),
            summary.get('kind'),
            summary.get('amount'),
            summary.get('damage'),
            summary.get('heal'),
            status.get('casts'),
            status.get('hits'),
            status.get('crit_rate'),
            filters.get('query'),
            tuple(sorted(self._expanded_refs)),
            refs,
        ))
