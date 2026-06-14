# -*- coding: utf-8 -*-
"""Entity-mode ACT trigger/timer manager panel."""

from __future__ import annotations

import json
import math
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_trigger_disable,
    act_trigger_enable,
    act_trigger_reload,
    act_trigger_status,
    act_trigger_test,
)
from gui_modules.sao_panel_components import (
    SP_SM, SP_MD, SP_XL,
    _pc, _accent, _accent_text,
    action_button, keep_canvas_scroll, rounded_panel,
    sao_entry, sao_scrollbar, section_card, status_badge,
)
from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules.sao_panel_ui import (
    _SAO_PANEL_ACCENT,
    _SAO_PANEL_BG,
    _SAO_PANEL_BODY_BG,
    _SAO_PANEL_BORDER,
    _SAO_PANEL_GOLD,
    _SAO_PANEL_HEADER_BG,
    _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_SEP,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _make_panel_close_button,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
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


def _format_number(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> str:
    num = _finite_float(value, default, lo=lo, hi=hi)
    if num == int(num):
        return str(int(num))
    return f"{num:.2f}".rstrip('0').rstrip('.')


class TriggerTimerManagerPanel:
    """SAO-styled Toplevel for ACT alert/timer rule management."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._list: Optional[tk.Frame] = None
        self._recent: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="0 RULES")
        self._status_var = tk.StringVar(value="Ready")
        self._last_status: Dict[str, Any] = {}
        self._last_render_sig = ""

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
        self._list = None
        self._recent = None
        self._last_render_sig = ""

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        try:
            status = act_trigger_status(self.owner)
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "triggers": [], "timers": [], "recent": [], "errors": [str(exc)]}
        self._last_status = dict(status or {})
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
        win.title('SAO ACT Trigger Timer Manager')
        win.geometry('960x862+190+140')
        win.minsize(660, 430)
        win.configure(bg=_SAO_PANEL_BG)
        try:
            win.overrideredirect(True)
            win.attributes('-alpha', 1.0)
        except Exception:
            pass
        try:
            _apply_window_icon(win)
        except Exception:
            pass
        header = _sao_panel_header(win, 'ACT TRIGGERS', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        bg = _pc('body_bg', _SAO_PANEL_BODY_BG)
        toolbar = tk.Frame(body, bg=bg)
        toolbar.pack(fill='x', padx=SP_MD, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=bg)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT TRIGGERS', bg=bg,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='TRIGGER / TIMER 触发计时', bg=bg,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))
        controls = tk.Frame(toolbar, bg=bg)
        controls.pack(side='right', anchor='s', pady=(0, 4))
        self._badge_frame = tk.Frame(controls, bg=bg)
        self._badge_frame.pack(side='left', padx=(0, 8))
        action_button(controls, '刷新', self.refresh).pack(side='left', padx=(0, 6))
        action_button(controls, '新建触发', None, kind='gold').pack(side='left', padx=(0, 6))
        _make_panel_close_button(controls, self.hide, bg=bg, flat=True).pack(side='left', padx=(6, 0))

        outer = tk.Frame(body, bg=bg)
        outer.pack(fill='both', expand=True, padx=SP_MD, pady=(0, SP_MD))
        left = tk.Frame(outer, bg=bg)
        left.pack(side='left', fill='both', expand=True, padx=(0, SP_SM))
        self._right = tk.Frame(outer, bg=bg, width=280)
        self._right.pack(side='right', fill='y')
        self._right.pack_propagate(False)

        canvas = tk.Canvas(left, bg=bg, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(left, canvas.yview)
        self._list = tk.Frame(canvas, bg=bg)
        self._list.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._list, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas
        win.protocol('WM_DELETE_WINDOW', self.hide)
        self._last_render_sig = ""

    def _render_status(self, status: Mapping[str, Any]) -> None:
        rules = self._display_rules(status)
        active_count = sum(1 for r in rules if r.get('enabled'))
        for child in list(self._badge_frame.winfo_children()):
            child.destroy()
        status_badge(self._badge_frame, f'{active_count} ACTIVE', kind='cyan').pack(side='left', padx=(0, SP_SM))
        if not hasattr(self, '_search_var'):
            self._search_var = tk.StringVar(value='')
        _placeholder = '搜索触发器'
        search = sao_entry(self._badge_frame, textvariable=self._search_var, width=16)
        search.pack(side='left', padx=(0, 4))
        try:
            is_empty = not self._search_var.get().strip()
            if is_empty:
                self._search_var.set(_placeholder)
            search.configure(fg=_pc('label_fg' if is_empty else 'value_fg', _SAO_PANEL_LABEL_FG if is_empty else _SAO_PANEL_VALUE_FG))
            def _on_focus_in(_e: Any, _ph: str = _placeholder) -> None:
                if self._search_var.get() == _ph:
                    self._search_var.set('')
                    search.configure(fg=_pc('value_fg', _SAO_PANEL_VALUE_FG))
            def _on_focus_out(_e: Any, _ph: str = _placeholder) -> None:
                if not self._search_var.get().strip():
                    self._search_var.set(_ph)
                    search.configure(fg=_pc('label_fg', _SAO_PANEL_LABEL_FG))
            search.bind('<FocusIn>', _on_focus_in)
            search.bind('<FocusOut>', _on_focus_out)
        except Exception:
            pass
        if self._list is None:
            return
        render_sig = self._render_signature(status)
        if render_sig == self._last_render_sig:
            return
        self._last_render_sig = render_sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._list)
        for child in list(self._list.winfo_children()):
            child.destroy()
        box = section_card(self._list, '▼ 触发器 Triggers', badge=str(len(rules)), accent='gold')
        box.pack(fill='x', pady=SP_SM)
        if not rules:
            bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
            tk.Label(box, text='未配置触发器', bg=bg, fg=_pc('label_fg', _SAO_PANEL_LABEL_FG), font=get_cjk_font(10), pady=20).pack(fill='x')
        else:
            for rule in rules:
                self._render_rule(box, rule)
        if self._right is not None:
            for child in list(self._right.winfo_children()):
                child.destroy()
            self._render_timers(status)
            self._render_controls()

    def _render_rule(self, parent: tk.Misc, rule: Mapping[str, Any]) -> None:
        bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
        rule_id = str(rule.get('id') or '')
        enabled = bool(rule.get('enabled'))
        label = str(rule.get('label') or rule_id)
        match_expr = str(rule.get('match') or '')
        message = str(rule.get('message') or '').strip()
        card = tk.Frame(parent, bg=bg, highlightthickness=1, highlightbackground=_pc('border', _SAO_PANEL_BORDER))
        card.pack(fill='x', pady=SP_SM, padx=SP_SM)
        top = tk.Frame(card, bg=bg)
        top.pack(fill='x', padx=SP_MD, pady=(SP_SM, 2))
        tk.Label(top, text=label, bg=bg, fg=_pc('value_fg', _SAO_PANEL_VALUE_FG),
                 font=get_cjk_font(10, True), anchor='w').pack(side='left', fill='x', expand=True)
        badge_text = 'ON' if enabled else 'OFF'
        badge_kind = 'ok' if enabled else 'danger'
        status_badge(top, badge_text, kind=badge_kind).pack(side='right')
        if match_expr:
            tk.Label(card, text=f'匹配 /{match_expr}/', bg=bg,
                     fg=_pc('label_fg', _SAO_PANEL_LABEL_FG), font=get_cjk_font(8), anchor='w').pack(fill='x', padx=SP_MD, pady=(0, 2))
        if message:
            tk.Label(card, text=f'动作 : {message}', bg=bg,
                     fg=_pc('value_fg', _SAO_PANEL_VALUE_FG), font=get_cjk_font(8), anchor='w').pack(fill='x', padx=SP_MD, pady=(0, SP_SM))

    _TIMER_COLORS = ['#ef684e', '#3bb4e5', '#5cc46a', '#e5b43b']

    @staticmethod
    def _timer_countdown(item: Mapping[str, Any]) -> str:
        """Format a timer item's remaining/duration as MM:SS countdown text."""
        import time as _time
        dur = _finite_float(item.get('duration_s') or item.get('threshold'), 0.0, lo=0.0)
        created = _finite_float(item.get('created_at'), 0.0, lo=0.0)
        if dur > 0 and created > 0:
            elapsed = max(0.0, _time.time() - created)
            remaining = max(0.0, dur - elapsed)
        elif dur > 0:
            remaining = dur
        else:
            remaining = 0.0
        mins = int(remaining) // 60
        secs = int(remaining) % 60
        return f'{mins:02d}:{secs:02d}'

    @staticmethod
    def _timer_ratio(item: Mapping[str, Any]) -> float:
        """Compute progress ratio (0..1) for a timer bar."""
        import time as _time
        dur = _finite_float(item.get('duration_s') or item.get('threshold'), 0.0, lo=0.0)
        if dur <= 0:
            return 0.0
        created = _finite_float(item.get('created_at'), 0.0, lo=0.0)
        if created > 0:
            elapsed = max(0.0, _time.time() - created)
            return max(0.0, min(1.0, (dur - elapsed) / dur))
        return 0.5

    def _render_timers(self, status: Mapping[str, Any]) -> None:
        bg = _pc('body_bg', _SAO_PANEL_BODY_BG)
        tk.Label(self._right, text='ACTIVE TIMERS 计时', bg=bg,
                 fg=_pc('gold', _SAO_PANEL_GOLD), font=get_cjk_font(10, True), anchor='w').pack(fill='x', pady=(0, SP_SM))
        timers = list(status.get('timers') or [])
        recent = list(status.get('recent') or [])
        items = timers + [r for r in recent if r.get('rule_id') not in {t.get('id') for t in timers}]
        for i, item in enumerate(items[:4]):
            label = str(item.get('label') or item.get('rule_id') or item.get('id') or '').strip()
            countdown = self._timer_countdown(item)
            color = self._TIMER_COLORS[i % len(self._TIMER_COLORS)]
            bar_frame = tk.Frame(self._right, bg=bg)
            bar_frame.pack(fill='x', pady=SP_SM)
            top = tk.Frame(bar_frame, bg=bg)
            top.pack(fill='x')
            tk.Label(top, text=label, bg=bg, fg=_pc('value_fg', _SAO_PANEL_VALUE_FG),
                     font=get_cjk_font(9, True), anchor='w').pack(side='left')
            tk.Label(top, text=countdown, bg=bg, fg=color, font=get_cjk_font(9, True), anchor='e').pack(side='right')
            bar = tk.Canvas(bar_frame, bg=bg, height=6, highlightthickness=0, bd=0)
            bar.pack(fill='x', pady=(2, 0))
            ratio = self._timer_ratio(item)
            bar.bind('<Configure>', lambda e, c=bar, r=ratio, col=color: (
                c.delete('all'),
                c.create_rectangle(0, 0, max(1, int(e.width * r)), 6, fill=col, outline=''),
                c.create_rectangle(max(1, int(e.width * r)), 0, e.width, 6, fill=_pc('border', _SAO_PANEL_BORDER), outline=''),
            ))

    def _render_controls(self) -> None:
        bg = _pc('body_bg', _SAO_PANEL_BODY_BG)
        tk.Label(self._right, text='CONTROL', bg=bg,
                 fg=_pc('gold', _SAO_PANEL_GOLD), font=get_cjk_font(10, True), anchor='w').pack(fill='x', pady=(SP_MD, SP_SM))
        grid = tk.Frame(self._right, bg=bg)
        grid.pack(fill='x')
        for col in range(2):
            grid.columnconfigure(col, weight=1)
        for i, (label, cmd) in enumerate([('导入', None), ('导出', None), ('测试', None), ('全部停用', self._reload)]):
            action_button(grid, label, cmd).grid(row=i // 2, column=i % 2, sticky='ew', padx=3, pady=3)

    def _reload(self) -> None:
        result = act_trigger_reload(self.owner)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Reload failed'))
        else:
            self._status_var.set('触发器已重载')
        self.refresh()

    def _enable(self, rule_id: str) -> None:
        result = act_trigger_enable(self.owner, rule_id)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Enable failed'))
        else:
            self._status_var.set(f'{rule_id} enabled')
        self.refresh()

    def _disable(self, rule_id: str) -> None:
        result = act_trigger_disable(self.owner, rule_id)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Disable failed'))
        else:
            self._status_var.set(f'{rule_id} disabled')
        self.refresh()

    def _test(self, rule_id: str) -> None:
        result = act_trigger_test(self.owner, rule_id)
        if isinstance(result, Mapping):
            count = len(result.get('events') or [])
            self._status_var.set(str(result.get('message') or f'Test events: {count}'))
        self.refresh()

    @staticmethod
    def _display_rules(status: Mapping[str, Any]) -> list[Mapping[str, Any]]:
        """Merge trigger rows with timer-only rows while avoiding duplicate IDs."""
        merged: list[Mapping[str, Any]] = []
        seen: set[str] = set()
        for source in (status.get('triggers') or [], status.get('timers') or []):
            if not isinstance(source, list):
                continue
            for rule in source:
                if not isinstance(rule, Mapping):
                    continue
                try:
                    key = str(rule.get('id') or '') or json.dumps(
                        rule,
                        sort_keys=True,
                        ensure_ascii=False,
                        default=str,
                    )
                except Exception:
                    key = repr(rule)
                if key in seen:
                    continue
                seen.add(key)
                merged.append(rule)
        return merged

    @classmethod
    def _render_signature(cls, status: Mapping[str, Any]) -> str:
        try:
            return json.dumps(
                {
                    "rules": cls._display_rules(status),
                    "recent": list(status.get('recent') or [])[:6],
                },
                sort_keys=True,
                ensure_ascii=False,
                default=str,
            )
        except Exception:
            return repr((cls._display_rules(status), list(status.get('recent') or [])[:6]))


__all__ = ["TriggerTimerManagerPanel"]
