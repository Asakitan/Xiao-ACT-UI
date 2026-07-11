# -*- coding: utf-8 -*-
# Entity-mode ACT timeline/VCR panel.

from __future__ import annotations

import json
import math
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
from gui_modules.sao_panel_components import (
    action_button,
    attach_tooltip,
    empty_state,
    fmt_dur,
    fmt_signed,
    keep_canvas_scroll,
    sao_entry,
    sao_option_menu,
    sao_scrollbar,
    status_badge,
    _pc,
)
from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules import sao_panel_ui as ui
from gui_modules.sao_panel_ui import (
    _apply_window_icon,
    _bind_panel_drag,
    _make_panel_close_button,
    _sao_panel_body,
    _sao_panel_header,
)


def _refresh_panel_palette() -> None:
    global _SAO_PANEL_ACCENT, _SAO_PANEL_BG, _SAO_PANEL_BODY_BG, _SAO_PANEL_BORDER
    global _SAO_PANEL_GOLD, _SAO_PANEL_LABEL_FG, _SAO_PANEL_VALUE_FG
    _SAO_PANEL_ACCENT = _pc('accent', ui._SAO_PANEL_ACCENT)
    _SAO_PANEL_BG = _pc('bg', ui._SAO_PANEL_BG)
    _SAO_PANEL_BODY_BG = _pc('body_bg', ui._SAO_PANEL_BODY_BG)
    _SAO_PANEL_BORDER = _pc('border', ui._SAO_PANEL_BORDER)
    _SAO_PANEL_GOLD = _pc('gold', ui._SAO_PANEL_GOLD)
    _SAO_PANEL_LABEL_FG = _pc('label_fg', ui._SAO_PANEL_LABEL_FG)
    _SAO_PANEL_VALUE_FG = _pc('value_fg', ui._SAO_PANEL_VALUE_FG)


_refresh_panel_palette()


def _finite_float(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> float:
    try:
        number = float(value)
    except Exception:
        number = float(default)
    if not math.isfinite(number):
        number = float(default)
    if lo is not None:
        number = max(lo, number)
    if hi is not None:
        number = min(hi, number)
    return number


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    number = int(_finite_float(value, float(default), lo=lo, hi=hi))
    return number


class TimelineVcrPanel:
    # SAO-styled compact timeline/VCR control panel for Entity/Tk.

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
        self._last_request_key: tuple[Any, ...] = ()
        self._last_events_sig = ""
        self._expanded_events: set[str] = set()

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
        self._reset_render_cache()

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        now = time.time()
        query = self._query_var.get()
        request_key = (query,)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_timeline_status(self.owner, limit=80, query=query)
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "events": [], "cursor_ms": 0, "speed": 1.0, "playing": False, "filters": {"query": query}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def play(self) -> Dict[str, Any]:
        speed = _finite_float(self._speed_var.get(), 1.0, lo=0.1, hi=8.0)
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
        speed = _finite_float(self._speed_var.get(), 1.0, lo=0.1, hi=8.0)
        return self._apply_result(act_timeline_set_speed(self.owner, speed=speed), f'SPEED {speed:g}x')

    def apply_filter(self) -> Dict[str, Any]:
        self._cancel_pending_filter()
        return self._apply_result(act_timeline_filter(self.owner, query=self._query_var.get()), 'FILTER APPLIED')

    def _cancel_pending_filter(self) -> None:
        pending = getattr(self, '_filter_after_id', None)
        self._filter_after_id = None
        if pending is not None and self._win is not None:
            try:
                self._win.after_cancel(pending)
            except Exception:
                pass

    def _schedule_filter(self) -> None:
        # 输入防抖：停止键入 300ms 后自动应用筛选（与 Web 端即时搜索一致）。
        if self._win is None:
            return
        self._cancel_pending_filter()
        try:
            self._filter_after_id = self._win.after(300, self.apply_filter)
        except Exception:
            self._filter_after_id = None

    def _apply_result(self, result: Mapping[str, Any], message: str) -> Dict[str, Any]:
        self._last_status = dict(result or {})
        self._last_refresh_at = time.time()
        self._last_request_key = ()
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or f'{message} FAILED'))
        else:
            self._status_var.set(message)
        self._render_status(self._last_status)
        return self._last_status

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        _refresh_panel_palette()
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Timeline VCR')
        win.geometry('960x862+250+175')
        win.minsize(680, 420)
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
        header = _sao_panel_header(win, 'ACT TIMELINE / VCR', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT REPLAY', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='TIMELINE / VCR 时间线', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))
        controls = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        controls.pack(side='right', anchor='center')
        self._badge_frame_vcr = tk.Frame(controls, bg=_SAO_PANEL_BODY_BG)
        self._badge_frame_vcr.pack(side='left', padx=(0, 8))
        action_button(controls, '刷新', self.refresh, kind='gold').pack(side='left', padx=(0, 6))
        action_button(controls, '导出片段', None).pack(side='left', padx=(0, 6))
        _make_panel_close_button(controls, self.hide, bg=_SAO_PANEL_BODY_BG, flat=True).pack(side='left', padx=(6, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='筛选', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        query_entry = sao_entry(control, textvariable=self._query_var, width=22)
        query_entry.pack(side='left', padx=(6, 10))
        # 输入即筛选（300ms 防抖，对齐 Web 端即时搜索），回车立即生效
        query_entry.bind('<KeyRelease>', lambda _e: self._schedule_filter())
        query_entry.bind('<Return>', lambda _e: self.apply_filter())
        # 倍速换成预设下拉，选中即生效（对齐 Web 端 0.5x/1x/2x/4x select）
        tk.Label(control, text='倍速', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_option_menu(control, self._speed_var, '0.5', '1.0', '2.0', '4.0',
                        command=lambda _v: self.set_speed()).pack(side='left', padx=(6, 10))
        for label, cmd, tip in (
            ('-1s', self.step_back, '后退 1 秒'),
            ('+1s', self.step_forward, '前进 1 秒'),
            ('0ms', self.seek_zero, '回到时间线起点'),
        ):
            btn = action_button(control, label, cmd)
            btn.pack(side='left', padx=(4, 0))
            attach_tooltip(btn, tip)

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
        self._events = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._events.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._events, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas
        win.protocol('WM_DELETE_WINDOW', self.hide)
        self._reset_render_cache()

    def _render_status(self, status: Mapping[str, Any]) -> None:
        _refresh_panel_palette()
        if hasattr(self, '_badge_frame_vcr'):
            for child in list(self._badge_frame_vcr.winfo_children()):
                child.destroy()
            playing = bool(status.get('playing'))
            status_badge(self._badge_frame_vcr, 'PLAYING' if playing else 'PAUSED',
                         kind='ok' if playing else 'gold').pack(side='left')
        events = list(status.get('events') or [])
        speed = _finite_float(status.get('speed'), 1.0, lo=0.1, hi=8.0)
        self._summary_var.set(
            f"{len(events)} EVENTS · cursor {fmt_dur(_finite_int(status.get('cursor_ms'), 0, lo=0))} · {speed:g}x"
        )
        errors = list(status.get('errors') or [])
        self._status_var.set(
            f"{'PLAYING' if status.get('playing') else 'PAUSED'} · encounter={status.get('encounter_id') or 'live'} · errors={len(errors)}"
        )
        if self._events is None:
            return
        sig = self._render_signature(status, events)
        if sig == self._last_events_sig:
            return
        self._last_events_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._events)
        for child in list(self._events.winfo_children()):
            child.destroy()
        self._render_transport(status, events)
        self._render_summary_pills(status, events)
        if not events:
            empty_state(self._events, '暂无 ACT 时间线事件', '开始识别或 replay 后会出现事件。').pack(fill='x', pady=8, padx=4)
            return
        self._render_keyframes_section(events)

    def _render_transport(self, status: Mapping[str, Any], events: list[Any]) -> None:
        # Playback transport bar: |<< [PLAY] >>| cursor ===o=== total.
        if self._events is None:
            return
        cursor_ms = _finite_int(status.get('cursor_ms'), 0, lo=0)
        # Total duration = max event time_ms relative to first event, or cursor
        base_ms = self._events_base_ms(events)
        total_ms = 0
        if events:
            max_time = max(_finite_int(e.get('time_ms'), 0, lo=0) for e in events if isinstance(e, Mapping))
            total_ms = max(0, max_time - base_ms)
        total_ms = max(total_ms, cursor_ms)

        bar = tk.Frame(self._events, bg=_SAO_PANEL_BODY_BG, highlightthickness=1,
                       highlightbackground=_SAO_PANEL_BORDER)
        bar.pack(fill='x', padx=4, pady=(0, 8))
        inner = tk.Frame(bar, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=10, pady=8)

        # Prev / Play / Next buttons
        btn_prev = action_button(inner, '◁◁', self.step_back)
        btn_prev.pack(side='left', padx=(0, 4))
        attach_tooltip(btn_prev, '后退 1 秒')
        playing = bool(status.get('playing'))
        btn_play = action_button(inner, '▶' if not playing else '❚❚',
                                 self.pause if playing else self.play,
                                 kind='gold')
        btn_play.pack(side='left', padx=(0, 4))
        attach_tooltip(btn_play, '暂停' if playing else '播放')
        btn_next = action_button(inner, '▷▷', self.step_forward)
        btn_next.pack(side='left', padx=(0, 8))
        attach_tooltip(btn_next, '前进 1 秒')

        # Current time label
        tk.Label(inner, text=self._fmt_kf_time(cursor_ms),
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_sao_font(11, True)).pack(side='left', padx=(4, 8))

        # End time label (right side)
        tk.Label(inner, text=self._fmt_kf_time(total_ms),
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                 font=get_sao_font(10)).pack(side='right', padx=(8, 0))

        # Slider track
        track_frame = tk.Frame(inner, bg=_SAO_PANEL_BODY_BG)
        track_frame.pack(side='left', fill='x', expand=True, padx=(4, 4))
        ratio = (cursor_ms / total_ms) if total_ms > 0 else 0.0
        ratio = max(0.0, min(1.0, ratio))
        track = tk.Canvas(track_frame, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG), height=14,
                          highlightthickness=0, bd=0)
        track.pack(fill='x')

        def _draw_track(_e=None):
            track.configure(bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG))
            w = track.winfo_width()
            if w < 10:
                return
            track.delete('all')
            y = 7
            # Filled portion (accent/gold)
            fill_w = max(0, int(w * ratio))
            if fill_w > 0:
                track.create_line(0, y, fill_w, y, fill=_pc('gold', ui._SAO_PANEL_GOLD), width=4)
            # Remaining portion (border/dim)
            if fill_w < w:
                track.create_line(fill_w, y, w, y, fill=_pc('border', ui._SAO_PANEL_BORDER), width=4)
            # Thumb circle
            cx = max(6, min(w - 6, fill_w))
            track.create_oval(cx - 6, y - 6, cx + 6, y + 6,
                              fill=_pc('accent', ui._SAO_PANEL_ACCENT), outline='')

        track.bind('<Configure>', _draw_track)
        track._sao_theme_repaint = _draw_track
        track.after(10, _draw_track)

    def _render_summary_pills(self, status: Mapping[str, Any], events: list[Any]) -> None:
        # Summary pills row: N 个事件 | N 个关键帧 | N 次死亡.
        if self._events is None:
            return
        n_events = len(events)
        # Keyframe = events with notable topics (not generic log/event)
        kf_topics = {'damage', 'death', 'boss', 'boss_state', 'boss_mechanic',
                     'boss_mechanic_skill', 'skill', 'actor_skill', 'heal',
                     'scene', 'encounter_started', 'encounter_finalized',
                     'shield', 'buff'}
        n_keyframes = sum(1 for e in events
                          if isinstance(e, Mapping) and str(e.get('topic') or '').lower() in kf_topics)
        n_deaths = sum(1 for e in events
                       if isinstance(e, Mapping) and str(e.get('topic') or '').lower() == 'death')
        row = tk.Frame(self._events, bg=_SAO_PANEL_BODY_BG)
        row.pack(fill='x', padx=4, pady=(0, 6))
        status_badge(row, f"{n_events} 个事件", kind='gold').pack(side='left', padx=(0, 6))
        if n_keyframes:
            status_badge(row, f"{n_keyframes} 个关键帧", kind='cyan').pack(side='left', padx=(0, 6))
        if n_deaths:
            status_badge(row, f"{n_deaths} 次死亡", kind='danger').pack(side='left', padx=(0, 6))

    def _render_keyframes_section(self, events: list[Any]) -> None:
        # KEYFRAMES section: colored dot + relative time + label per event.
        if self._events is None:
            return
        # Section header
        hdr = tk.Frame(self._events, bg=_SAO_PANEL_BODY_BG)
        hdr.pack(fill='x', padx=4, pady=(4, 6))
        tk.Label(hdr, text='KEYFRAMES 关键帧', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_ACCENT, font=get_sao_font(10, True),
                 anchor='w').pack(fill='x')
        # Compute base_ms for relative times
        base_ms = self._events_base_ms(events)
        for event in events[:80]:
            self._render_keyframe(event, parent=self._events, base_ms=base_ms)

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
            font=get_cjk_font(10),
            pady=28,
        ).pack(fill='x')

    # ── Keyframe dot color mapping ──
    _KF_DOT_COLORS: Dict[str, str] = {
        'encounter_started': '#dea620',   # gold — battle start
        'encounter_finalized': '#dea620',
        'scene': '#dea620',
        'damage': '#5cc46a',              # green — damage/crit
        'skill': '#5cc46a',
        'actor_skill': '#5cc46a',
        'heal': '#5cc46a',
        'boss': '#e08a3c',               # orange — boss action
        'boss_state': '#e08a3c',
        'boss_mechanic': '#e08a3c',
        'boss_mechanic_skill': '#e08a3c',
        'death': '#ef684e',              # red — death
        'shield': '#68e4ff',             # cyan — defensive/milestone
        'buff': '#68e4ff',
    }

    def _render_keyframe(self, event: Mapping[str, Any], parent: Optional[tk.Misc] = None,
                         base_ms: int = 0) -> None:
        # Render a single keyframe card: colored dot + relative time + label.
        parent = parent or self._events
        if parent is None:
            return
        topic = str(event.get('topic') or '-')
        event_id = str(event.get('id') or f"{topic}:{event.get('time_ms')}")
        open_event = event_id in self._expanded_events
        dot_color = self._KF_DOT_COLORS.get(topic.lower(), _SAO_PANEL_ACCENT)

        # Relative time from encounter base
        event_ms = _finite_int(event.get('time_ms'), 0, lo=0)
        rel_ms = max(0, event_ms - base_ms)

        # Card container with left border
        card = tk.Frame(parent, bg=_SAO_PANEL_BODY_BG, highlightthickness=1,
                        highlightbackground=_SAO_PANEL_BORDER,
                        cursor='hand2')
        card.pack(fill='x', padx=4, pady=3)
        inner = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=8, pady=7)

        # Colored dot (canvas circle)
        dot = tk.Canvas(inner, width=10, height=10, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG),
                        highlightthickness=0, bd=0)
        def _draw_dot(_event: Any = None) -> None:
            dot.configure(bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG))
            dot.delete('all')
            dot.create_oval(1, 1, 9, 9, fill=dot_color, outline='')
        dot._sao_theme_repaint = _draw_dot
        _draw_dot()
        dot.pack(side='left', padx=(0, 8), pady=2)

        # Timestamp in gold (MM:SS.d relative format)
        tk.Label(inner, text=self._fmt_kf_time(rel_ms),
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_sao_font(10, True)).pack(side='left', padx=(0, 14))

        # Label text
        tk.Label(inner, text=str(event.get('label') or topic),
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                 font=get_cjk_font(10), anchor='w').pack(side='left', fill='x', expand=True)

        # Click to expand
        def _toggle(key=event_id):
            self._toggle_event(key)
        card.bind('<Button-1>', lambda _e: _toggle())
        for child in inner.winfo_children():
            child.bind('<Button-1>', lambda _e: _toggle())

        if open_event:
            payload = json.dumps(event.get('payload') or {}, ensure_ascii=False, default=str)
            tk.Label(parent, text=payload, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     anchor='w', justify='left', wraplength=760,
                     font=get_cjk_font(8)).pack(fill='x', padx=22, pady=(0, 6))

    @staticmethod
    def _fmt_kf_time(ms: int) -> str:
        # Relative offsets are already deltas, matching the shared formatter API.
        return fmt_signed(ms)

    @staticmethod
    def _events_base_ms(events: list[Any]) -> int:
        # Return the earliest event time_ms as the encounter base for relative times.
        base = 0
        for e in events:
            if isinstance(e, Mapping):
                t = _finite_int(e.get('time_ms'), 0, lo=0)
                if t > 0 and (base == 0 or t < base):
                    base = t
        return base

    def _toggle_event(self, event_id: str) -> None:
        if event_id in self._expanded_events:
            self._expanded_events.remove(event_id)
        else:
            self._expanded_events.add(event_id)
        self._reset_render_cache()
        self._render_status(self._last_status)

    def _reset_render_cache(self) -> None:
        self._last_events_sig = ""

    def _render_signature(self, status: Mapping[str, Any], events: list[Any]) -> str:
        try:
            return json.dumps(
                {
                    "events": self._events_signature(events),
                    "expanded": sorted(self._expanded_events),
                    "cursor_ms": status.get('cursor_ms'),
                    "speed": status.get('speed'),
                    "playing": bool(status.get('playing')),
                    "encounter_id": status.get('encounter_id'),
                    "errors": list(status.get('errors') or []),
                },
                sort_keys=True,
                ensure_ascii=False,
                default=str,
            )
        except Exception:
            return repr((
                self._events_signature(events),
                sorted(self._expanded_events),
                status.get('cursor_ms'),
                status.get('speed'),
                status.get('playing'),
                status.get('encounter_id'),
                status.get('errors'),
            ))

    @staticmethod
    def _events_signature(events: list[Any]) -> str:
        parts: list[dict[str, Any]] = []
        for event in events[:80]:
            if not isinstance(event, Mapping):
                continue
            parts.append({
                "id": event.get('id'),
                "topic": event.get('topic'),
                "time_ms": event.get('time_ms'),
                "label": event.get('label'),
                "value": event.get('value'),
                "source": event.get('source'),
                "payload": event.get('payload'),
            })
        return json.dumps({"count": len(events), "rows": parts}, sort_keys=True, ensure_ascii=False, default=str)

    @staticmethod
    def _fmt(value: Any) -> str:
        try:
            number = float(value or 0.0)
        except Exception:
            return str(value or '')
        if not math.isfinite(number):
            number = 0.0
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        return str(int(number)) if number == int(number) else f"{number:.2f}"
