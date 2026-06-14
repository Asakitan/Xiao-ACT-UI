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
from gui_modules.sao_panel_components import keep_canvas_scroll, sao_scrollbar
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
        header = _sao_panel_header(win, 'ACT TRIGGERS / TIMERS', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'ALERT SDK').pack(side='left')
        for label, cmd in (
            ('刷新', self.refresh),
            ('重载 Reload', self._reload),
            ('×', self.hide),
        ):
            tk.Button(
                toolbar,
                text=label,
                command=cmd,
                bg=_SAO_PANEL_HEADER_BG,
                fg=_SAO_PANEL_HEADER_FG,
                activebackground=_SAO_PANEL_ACCENT,
                activeforeground='white',
                relief='flat',
                bd=0,
                padx=10,
                pady=4,
            ).pack(side='right', padx=(6, 0))

        # 按钮先 pack — 窄窗下 summary 不挤按钮(后包者只分剩余空间)
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=get_cjk_font(10, True),
        ).pack(side='left', padx=(12, 0))
        tk.Label(
            body,
            textvariable=self._status_var,
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            font=get_cjk_font(9),
        ).pack(fill='x', padx=12, pady=(0, 6))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=12, pady=(0, 12))

        canvas = tk.Canvas(outer, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(outer, canvas.yview)
        self._list = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
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
        timers = list(status.get('timers') or [])
        self._summary_var.set(f'{len(rules)} RULES / {len(timers)} TIMERS')
        errors = status.get('errors') or []
        message = status.get('message') or ('OK' if status.get('ok', True) else 'Trigger manager unavailable')
        if errors:
            message = f"{message} · errors={len(errors)}"
        self._status_var.set(str(message))
        if self._list is None:
            return
        render_sig = self._render_signature(status)
        if render_sig == self._last_render_sig:
            return
        self._last_render_sig = render_sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._list)
        for child in list(self._list.winfo_children()):
            child.destroy()
        if not rules:
            self._render_empty()
            return
        for rule in rules:
            self._render_rule(rule)
        self._render_recent(status)

    def _render_empty(self) -> None:
        if self._list is None:
            return
        box = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(
            box,
            text='未配置 ACT 触发器\n在 settings.json 的 act_trigger_rules 中添加规则后刷新。',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=get_cjk_font(10),
            pady=28,
        ).pack(fill='x')

    def _render_rule(self, rule: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        rule_id = str(rule.get('id') or '')
        enabled = bool(rule.get('enabled'))
        is_timer = str(rule.get('type') or '') == 'elapsed_s'
        border = _SAO_PANEL_GOLD if is_timer else (_SAO_PANEL_BORDER if enabled else _SAO_PANEL_SEP)
        card = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=border)
        card.pack(fill='x', pady=6, padx=4)

        top = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        top.pack(fill='x', padx=10, pady=(8, 2))
        tk.Label(
            top,
            text=str(rule.get('label') or rule_id),
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_VALUE_FG,
            anchor='w',
            font=get_cjk_font(11, True),
        ).pack(side='left', fill='x', expand=True)
        _sao_pill(top, 'TIMER' if is_timer else ('ENABLED' if enabled else 'DISABLED')).pack(side='right')

        meta = tk.Label(
            card,
            text=self._format_rule(rule),
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            anchor='w',
            justify='left',
            font=('Consolas', 9),
        )
        meta.pack(fill='x', padx=10, pady=(2, 4))

        message = str(rule.get('message') or '').strip()
        if message:
            tk.Label(
                card,
                text=message,
                bg=_SAO_PANEL_BODY_BG,
                fg=_SAO_PANEL_VALUE_FG,
                anchor='w',
                justify='left',
                wraplength=730,
                font=get_cjk_font(9),
                padx=8,
                pady=5,
            ).pack(fill='x', padx=10, pady=(0, 6))

        actions = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        actions.pack(fill='x', padx=10, pady=(0, 9))
        self._action_button(actions, '启用', lambda rid=rule_id: self._enable(rid), enabled=not enabled)
        self._action_button(actions, '禁用', lambda rid=rule_id: self._disable(rid), enabled=enabled)
        self._action_button(actions, '测试', lambda rid=rule_id: self._test(rid), enabled=True)

    def _render_recent(self, status: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        events = list(status.get('recent') or [])[:6]
        if not events:
            return
        box = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=(10, 4), padx=4)
        tk.Label(
            box,
            text='RECENT EVENTS',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            anchor='w',
            font=get_cjk_font(10, True),
            padx=10,
            pady=6,
        ).pack(fill='x')
        for event in events:
            text = f"{event.get('label') or event.get('rule_id')}: {event.get('message') or ''}"
            tk.Label(
                box,
                text=text,
                bg=_SAO_PANEL_BODY_BG,
                fg=_SAO_PANEL_LABEL_FG,
                anchor='w',
                justify='left',
                wraplength=730,
                font=('Consolas', 8),
                padx=10,
                pady=3,
            ).pack(fill='x')

    def _format_rule(self, rule: Mapping[str, Any]) -> str:
        threshold = _format_number(rule.get('threshold'), 0, lo=0)
        cooldown = _format_number(rule.get('cooldown_s'), 0, lo=0, hi=86400)
        return (
            f"id={rule.get('id') or '-'}  type={rule.get('type') or '-'}  "
            f"threshold={threshold}  match={rule.get('match') or '-'}\n"
            f"cooldown={cooldown}s  "
            f"once={'yes' if rule.get('once_per_encounter') else 'no'}  "
            f"severity={rule.get('severity') or 'info'}"
        )

    def _action_button(self, parent: tk.Frame, text: str, command: Any, *, enabled: bool = True) -> None:
        tk.Button(
            parent,
            text=text,
            command=command,
            state=('normal' if enabled else 'disabled'),
            bg=_SAO_PANEL_HEADER_BG,
            fg=_SAO_PANEL_HEADER_FG,
            disabledforeground=_SAO_PANEL_LABEL_FG,
            activebackground=_SAO_PANEL_ACCENT,
            activeforeground='white',
            relief='flat',
            bd=0,
            padx=9,
            pady=3,
        ).pack(side='left', padx=(0, 7))

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
