# -*- coding: utf-8 -*-
"""Entity-mode ACT timeline/VCR panel."""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_timeline_filter,
    act_timeline_pause,
    act_timeline_play,
    act_timeline_seek,
    act_timeline_set_speed,
    act_timeline_status,
    act_timeline_step,
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


class TimelineVcrPanel:
    """SAO-styled compact timeline/VCR control panel for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._events: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="TIMELINE: --")
        self._status_var = tk.StringVar(value="Ready")
        self._query_var = tk.StringVar(value="")
        self._speed_var = tk.StringVar(value="1.0")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_events_sig = ""

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
        self._events = None

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        now = time.time()
        if self._last_status and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_timeline_status(self.owner, limit=80, query=self._query_var.get())
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "events": [], "cursor_ms": 0, "speed": 1.0, "playing": False, "filters": {"query": self._query_var.get()}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._render_status(self._last_status)
        return self._last_status

    def play(self) -> Dict[str, Any]:
        try:
            speed = float(self._speed_var.get() or 1.0)
        except Exception:
            speed = 1.0
        return self._apply_result(act_timeline_play(self.owner, speed=speed), 'PLAYING')

    def pause(self) -> Dict[str, Any]:
        return self._apply_result(act_timeline_pause(self.owner), 'PAUSED')

    def step_back(self) -> Dict[str, Any]:
        return self._apply_result(act_timeline_step(self.owner, delta_ms=-1000), 'STEP -1s')

    def step_forward(self) -> Dict[str, Any]:
        return self._apply_result(act_timeline_step(self.owner, delta_ms=1000), 'STEP +1s')

    def seek_zero(self) -> Dict[str, Any]:
        return self._apply_result(act_timeline_seek(self.owner, cursor_ms=0), 'SEEK 0ms')

    def set_speed(self) -> Dict[str, Any]:
        try:
            speed = float(self._speed_var.get() or 1.0)
        except Exception:
            speed = 1.0
        return self._apply_result(act_timeline_set_speed(self.owner, speed=speed), f'SPEED {speed:g}x')

    def apply_filter(self) -> Dict[str, Any]:
        return self._apply_result(act_timeline_filter(self.owner, query=self._query_var.get()), 'FILTER APPLIED')

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
        win.title('SAO ACT Timeline VCR')
        win.geometry('840x540+250+175')
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
        header = _sao_panel_header(win, 'ACT TIMELINE / VCR', on_close=self.hide)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'VCR').pack(side='left')
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(side='left', padx=(12, 0))
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('播放 Play', self.play),
            ('暂停 Pause', self.pause),
            ('关闭 Close', self.hide),
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

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='Filter', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=22).pack(side='left', padx=(6, 6))
        tk.Button(control, text='应用 Filter', command=self.apply_filter).pack(side='left', padx=(0, 10))
        tk.Label(control, text='Speed', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._speed_var, width=5).pack(side='left', padx=(6, 6))
        tk.Button(control, text='设置 Speed', command=self.set_speed).pack(side='left', padx=(0, 10))
        for label, cmd in (
            ('-1s', self.step_back),
            ('+1s', self.step_forward),
            ('0ms', self.seek_zero),
        ):
            tk.Button(control, text=label, command=cmd).pack(side='left', padx=(4, 0))

        tk.Label(
            body,
            textvariable=self._status_var,
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            font=('Segoe UI', 9),
        ).pack(fill='x', padx=12, pady=(0, 6))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=12, pady=(0, 12))
        canvas = tk.Canvas(outer, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = tk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        self._events = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._events.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=self._events, anchor='nw')
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        events = list(status.get('events') or [])
        self._summary_var.set(
            f"{len(events)} EVENTS · {int(status.get('cursor_ms') or 0)}ms · {float(status.get('speed') or 1.0):g}x"
        )
        errors = list(status.get('errors') or [])
        self._status_var.set(
            f"{'PLAYING' if status.get('playing') else 'PAUSED'} · encounter={status.get('encounter_id') or 'live'} · errors={len(errors)}"
        )
        if self._events is None:
            return
        sig = self._events_signature(events)
        if sig == self._last_events_sig:
            return
        self._last_events_sig = sig
        for child in list(self._events.winfo_children()):
            child.destroy()
        if not events:
            self._render_empty()
            return
        for event in events[:80]:
            self._render_event(event)

    def _render_empty(self) -> None:
        if self._events is None:
            return
        box = tk.Frame(self._events, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(
            box,
            text='暂无 ACT 时间线事件\n开始识别或 replay 后会出现事件。',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=('Segoe UI', 10),
            pady=28,
        ).pack(fill='x')

    def _render_event(self, event: Mapping[str, Any]) -> None:
        if self._events is None:
            return
        topic = str(event.get('topic') or '-')
        card = tk.Frame(self._events, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        card.pack(fill='x', pady=5, padx=4)
        top = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        top.pack(fill='x', padx=10, pady=(8, 2))
        tk.Label(top, text=str(event.get('label') or topic), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, anchor='w', font=('Segoe UI', 10, 'bold')).pack(side='left', fill='x', expand=True)
        _sao_pill(top, topic.upper()).pack(side='right')
        meta = f"t={int(event.get('time_ms') or 0)}ms · source={event.get('source') or '-'} · value={event.get('value') or ''}"
        tk.Label(card, text=meta, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, anchor='w', justify='left', font=('Segoe UI', 9)).pack(fill='x', padx=10, pady=(0, 8))

    @staticmethod
    def _events_signature(events: list[Any]) -> str:
        parts = []
        for event in events[:80]:
            if not isinstance(event, Mapping):
                continue
            parts.append((event.get('id'), event.get('topic'), event.get('time_ms'), event.get('label'), event.get('value')))
        return repr(parts)
