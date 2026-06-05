# -*- coding: utf-8 -*-
"""Entity-mode ACT graph/timeseries panel."""

from __future__ import annotations

import json
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_graph_timeseries_export,
    act_graph_timeseries_filter,
    act_graph_timeseries_select_metric,
    act_graph_timeseries_status,
    act_graph_timeseries_zoom,
)
from gui_modules.sao_panel_components import (
    action_button,
    aggregate_row,
    empty_state,
    fmt_clock,
    fmt_dur,
    fmt_rel,
    metric_tile,
    section_card,
    status_badge,
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


_BAR_COLORS = {
    "damage": "#f3af12",
    "heal": "#2ebf86",
    "event_count": "#35bfe8",
    "boss_hp_pct": "#e85c7a",
}


class GraphTimeseriesPanel:
    """SAO-styled compact graph/timeseries panel for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="GRAPH: --")
        self._status_var = tk.StringVar(value="Ready")
        self._metric_var = tk.StringVar(value="damage")
        self._topic_var = tk.StringVar(value="")
        self._query_var = tk.StringVar(value="")
        self._zoom_var = tk.StringVar(value="0")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_series_sig = ""
        self._show_raw_points = tk.BooleanVar(value=False)

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
            zoom_ms = int(self._zoom_var.get() or 0)
        except Exception:
            zoom_ms = 0
        try:
            status = act_graph_timeseries_status(
                self.owner,
                metric=self._metric_var.get(),
                limit=120,
                query=self._query_var.get(),
                topic=self._topic_var.get(),
                time_range_ms=zoom_ms,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "series": {}, "metrics": [], "time_range_ms": 0, "filters": {"query": self._query_var.get(), "topic": self._topic_var.get()}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._render_status(self._last_status)
        return self._last_status

    def select_metric(self) -> Dict[str, Any]:
        return self._apply_result(act_graph_timeseries_select_metric(self.owner, metric=self._metric_var.get(), limit=120), 'METRIC SELECTED')

    def zoom(self) -> Dict[str, Any]:
        try:
            zoom_ms = int(self._zoom_var.get() or 0)
        except Exception:
            zoom_ms = 0
        return self._apply_result(act_graph_timeseries_zoom(self.owner, time_range_ms=zoom_ms, limit=120), f'ZOOM {zoom_ms}ms')

    def filter(self) -> Dict[str, Any]:
        return self._apply_result(act_graph_timeseries_filter(self.owner, query=self._query_var.get(), topic=self._topic_var.get(), limit=120), 'FILTER APPLIED')

    def export_json(self) -> Dict[str, Any]:
        try:
            result = act_graph_timeseries_export(
                self.owner,
                metric=self._metric_var.get(),
                limit=120,
                query=self._query_var.get(),
                topic=self._topic_var.get(),
            )
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "text": json.dumps(self._last_status, ensure_ascii=False, indent=2)}
        text = str(result.get('text') or json.dumps(self._last_status, ensure_ascii=False, indent=2))
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Graph JSON copied to clipboard')
        except Exception as exc:
            self._status_var.set(str(exc))
            result = dict(result)
            result.update({"ok": False, "message": str(exc)})
        return dict(result or {})

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
        win.title('SAO ACT Graph Timeseries')
        win.geometry('820x540+260+150')
        win.minsize(680, 410)
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
        header = _sao_panel_header(win, 'ACT GRAPH / TIMESERIES', on_close=self.hide)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'GRAPH').pack(side='left')
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(side='left', padx=(12, 0))
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('导出 Export', self.export_json),
            ('关闭 Close', self.hide),
        ):
            action_button(toolbar, label, cmd, kind='cyan' if '导出' in label else 'gold').pack(side='right', padx=(6, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='Metric', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._metric_var, 'damage', 'heal', 'event_count', 'boss_hp_pct', command=lambda _v: self.select_metric()).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Topic', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._topic_var, '', 'damage', 'heal', 'boss', 'skill', command=lambda _v: self.filter()).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Search', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=18).pack(side='left', padx=(6, 8))
        tk.Button(control, text='过滤 Filter', command=self.filter).pack(side='left', padx=(0, 8))
        tk.Label(control, text='Range ms', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._zoom_var, '0', '5000', '15000', '30000', '60000', command=lambda _v: self.zoom()).pack(side='left', padx=(6, 0))
        tk.Checkbutton(
            control,
            text='RAW points',
            variable=self._show_raw_points,
            command=self._toggle_raw_points,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            selectcolor=_SAO_PANEL_HEADER_BG,
            activebackground=_SAO_PANEL_BODY_BG,
            activeforeground=_SAO_PANEL_GOLD,
            font=('Segoe UI', 9),
        ).pack(side='left', padx=(10, 0))

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
        self._rows = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG)
        self._rows.pack(fill='both', expand=True)
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        metric = str(status.get('selected_metric') or self._metric_var.get() or 'damage')
        series = status.get('series') if isinstance(status.get('series'), Mapping) else {}
        selected = series.get(metric) if isinstance(series.get(metric), Mapping) else {}
        points = list(selected.get('points') or [])
        latest = points[-1].get('value') if points else 0
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        self._summary_var.set(
            f"{metric.upper()} · {len(points)} PTS · {self._fmt(latest)} · {fmt_dur(status.get('time_range_ms'))}"
        )
        errors = list(status.get('errors') or [])
        self._status_var.set(
            f"encounter={status.get('encounter_id') or 'live'} · topic={filters.get('topic') or 'ALL'} · query={filters.get('query') or '-'} · errors={len(errors)}"
        )
        if self._rows is None:
            return
        sig = self._series_signature(metric, points, status) + repr(bool(self._show_raw_points.get()))
        if sig == self._last_series_sig:
            return
        self._last_series_sig = sig
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_metrics(status, metric, points, latest)
        if not points:
            empty_state(self._rows, '暂无 ACT 图表数据', '开始识别或 replay 后会出现曲线点。').pack(fill='both', expand=True, pady=8, padx=4)
            return
        self._render_chart(metric, points)
        self._render_points(metric, points)

    def _toggle_raw_points(self) -> None:
        self._last_series_sig = ""
        self._render_status(self._last_status)

    def _render_metrics(self, status: Mapping[str, Any], metric: str, points: list[Mapping[str, Any]], latest: Any) -> None:
        if self._rows is None:
            return
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        first_ms = int(points[0].get('time_ms') or 0) if points else 0
        last_ms = int(points[-1].get('time_ms') or 0) if points else 0
        span_sub = f"{fmt_clock(first_ms)} · {fmt_dur(last_ms - first_ms)}" if first_ms else '--'
        items = (
            ('Metric', metric.upper(), f"topic {filters.get('topic') or 'ALL'}", 'gold'),
            ('Latest', self._fmt(latest), f"{len(points)} points", 'cyan'),
            ('Range', fmt_dur(status.get('time_range_ms')), span_sub, 'cyan'),
            ('Rows', int(status.get('row_count') or len(points)), f"raw {'ON' if self._show_raw_points.get() else 'OFF'}", 'gold'),
        )
        for label, value, sub, accent in items:
            metric_tile(grid, label, value, sub=str(sub), accent=accent).pack(side='left', fill='x', expand=True, padx=3)
        badges = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        badges.pack(fill='x', padx=4, pady=(0, 8))
        status_badge(badges, f"QUERY {filters.get('query') or '-'}", kind='cyan').pack(side='left', padx=(0, 6))
        status_badge(badges, f"ERRORS {len(status.get('errors') or [])}", kind='danger' if status.get('errors') else 'cyan').pack(side='left', padx=(0, 6))

    def _render_empty(self) -> None:
        if self._rows is None:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='both', expand=True, pady=8, padx=4)
        tk.Label(
            box,
            text='暂无 ACT 图表数据\n开始识别或 replay 后会出现曲线点。',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=('Segoe UI', 10),
            pady=48,
        ).pack(fill='both', expand=True)

    def _render_chart(self, metric: str, points: list[Mapping[str, Any]]) -> None:
        if self._rows is None:
            return
        chart = section_card(self._rows, '趋势图表', subtitle='保留图形主视图；下方点位列表默认只显示 compact rows。', badge=metric.upper())
        chart.pack(fill='x', pady=(0, 8), padx=4)
        chart_body = tk.Frame(chart, bg=_SAO_PANEL_BODY_BG)
        chart_body.pack(fill='x', padx=8, pady=8)
        max_value = max(1.0, *[float(point.get('value') or 0.0) for point in points])
        color = _BAR_COLORS.get(metric, _SAO_PANEL_ACCENT)
        first_ms = int(points[0].get('time_ms') or 0) if points else 0
        for point in points[-24:]:
            row = tk.Frame(chart_body, bg=_SAO_PANEL_BODY_BG)
            row.pack(fill='x', padx=8, pady=3)
            time_label = fmt_rel(point.get('time_ms'), first_ms)
            value = float(point.get('value') or 0.0)
            tk.Label(row, text=time_label, width=10, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8)).pack(side='left')
            bar_wrap = tk.Frame(row, bg='#eeeeee', height=8)
            bar_wrap.pack(side='left', fill='x', expand=True, padx=(6, 8))
            width = max(4, min(220, int(220 * value / max_value)))
            tk.Frame(bar_wrap, bg=color, width=width, height=8).pack(side='left')
            tk.Label(row, text=self._fmt(value), width=10, anchor='e', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Segoe UI', 8, 'bold')).pack(side='left')

    def _render_points(self, metric: str, points: list[Mapping[str, Any]]) -> None:
        if self._rows is None:
            return
        box = section_card(self._rows, '点位摘要', subtitle='按时间倒序显示关键点；raw row id 只在 RAW 模式显示。', badge=str(len(points)))
        box.pack(fill='x', pady=(0, 2), padx=4)
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=8, pady=8)
        max_value = max(1.0, *[float(point.get('value') or 0.0) for point in points])
        first_ms = int(points[0].get('time_ms') or 0) if points else 0
        for idx, point in enumerate(reversed(points[-16:])):
            row_id = str(point.get('row_id') or '-')
            time_ms = int(point.get('time_ms') or 0)
            topic = str(point.get('topic') or '-').upper()
            meta = f"{topic} · {fmt_clock(time_ms)} ({fmt_rel(time_ms, first_ms)})"
            if self._show_raw_points.get():
                meta += f" · row={row_id}"
            aggregate_row(
                body,
                title=f"{metric.upper()} {self._fmt(point.get('value'))}",
                meta=meta,
                value=row_id if self._show_raw_points.get() else self._fmt(point.get('value')),
                ratio=float(point.get('value') or 0.0) / max_value if max_value else 0.0,
                accent='gold' if metric == 'damage' else 'cyan',
                zebra=bool(idx % 2),
                command=lambda t=time_ms, tp=str(point.get('topic') or ''): self._open_action_log_at(t, tp),
            ).pack(fill='x', pady=2)

    def _open_action_log_at(self, time_ms: int, topic: str) -> None:
        """Drill a graph point into the Action Log focused at that point's time."""
        owner = self.owner
        try:
            panel = getattr(owner, '_act_action_log_panel', None)
            visible = bool(panel and getattr(panel, 'is_visible', lambda: False)())
            if not visible:
                toggle = getattr(owner, '_toggle_act_action_log_panel', None)
                if callable(toggle):
                    toggle()
                panel = getattr(owner, '_act_action_log_panel', None)
            if panel is None:
                self._status_var.set('Action Log panel unavailable')
                return
            try:
                panel._cursor_var.set(str(int(time_ms or 0)))
                if topic:
                    panel._topic_var.set(str(topic))
                jump = getattr(panel, 'jump_to_time', None) or getattr(panel, 'refresh', None)
                if callable(jump):
                    jump()
            except Exception:
                pass
            self._status_var.set(f'Action Log @ {fmt_clock(time_ms)}')
        except Exception as exc:
            try:
                self._status_var.set(str(exc))
            except Exception:
                pass

    @staticmethod
    def _fmt(value: Any) -> str:
        try:
            number = float(value or 0.0)
        except Exception:
            return str(value or '0')
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        if number == int(number):
            return str(int(number))
        return f"{number:.2f}"

    @staticmethod
    def _series_signature(metric: str, points: list[Mapping[str, Any]], status: Mapping[str, Any]) -> str:
        parts = []
        for point in points[-24:]:
            parts.append((point.get('time_ms'), point.get('topic'), point.get('value'), point.get('row_id')))
        return repr((metric, int(status.get('time_range_ms') or 0), parts))
