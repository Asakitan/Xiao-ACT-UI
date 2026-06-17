# -*- coding: utf-8 -*-
"""Entity-mode ACT graph/timeseries panel."""

from __future__ import annotations

import json
import math
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
    empty_state,
    fmt_clock,
    sao_option_menu,
    section_card,
    status_badge,
)
from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules.sao_panel_ui import (
    _SAO_PANEL_ACCENT,
    _SAO_PANEL_BG,
    _SAO_PANEL_BODY_BG,
    _SAO_PANEL_BORDER,
    _SAO_PANEL_GOLD,
    _SAO_PANEL_HEADER_BG,
    _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _make_panel_close_button,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
)


_BAR_COLORS = {
    "damage": "#f3af12",
    "heal": "#2ebf86",
    "event_count": "#35bfe8",
    "target_hp_pct": "#e85c7a",
}

_DEFAULT_METRICS = (
    {"id": "damage", "label": "Damage"},
    {"id": "heal", "label": "Heal"},
    {"id": "event_count", "label": "Events"},
    {"id": "target_hp_pct", "label": "Target HP %"},
)


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
    return int(_finite_float(value, float(default), lo=lo, hi=hi))


def _mapping_points(value: Any) -> list[Mapping[str, Any]]:
    if not isinstance(value, list):
        return []
    return [point for point in value if isinstance(point, Mapping)]


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
        self._last_request_key: tuple[Any, ...] = ()
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
        try:
            zoom_ms = int(self._zoom_var.get() or 0)
        except Exception:
            zoom_ms = 0
        metric = self._metric_var.get()
        query = self._query_var.get()
        topic = self._topic_var.get()
        request_key = (metric, query, topic, zoom_ms)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_graph_timeseries_status(
                self.owner,
                metric=metric,
                limit=120,
                query=query,
                topic=topic,
                time_range_ms=zoom_ms,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "series": {}, "metrics": [], "time_range_ms": 0, "filters": {"query": query, "topic": topic}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
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
        export_failed = bool(result.get('ok') is False)
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            if export_failed:
                self._status_var.set(
                    f"Export failed: {result.get('message') or 'unknown error'} — fallback JSON copied")
            else:
                self._status_var.set('Graph JSON copied to clipboard')
        except Exception as exc:
            self._status_var.set(str(exc))
            result = dict(result)
            result.update({"ok": False, "message": str(exc)})
        return dict(result or {})

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
        win.title('SAO ACT Graph Timeseries')
        win.geometry('960x862+260+150')
        win.minsize(680, 410)
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
        header = _sao_panel_header(win, 'ACT GRAPH / TIMESERIES', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        # ── toolbar: dual-line title + READY badge + controls ──
        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(7, 10))

        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT TIMESERIES', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='GRAPH / 图表曲线', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))

        controls = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        controls.pack(side='right', anchor='center')
        tk.Label(controls, text='范围', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                 font=get_cjk_font(9)).pack(side='left')
        sao_option_menu(controls, self._zoom_var, '0', '5000', '15000', '30000', '60000',
                        command=lambda _v: self.zoom()).pack(side='left', padx=(0, 8))
        tk.Label(controls, text='指标', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                 font=get_cjk_font(9)).pack(side='left')
        sao_option_menu(controls, self._metric_var, 'damage', 'heal', 'event_count', 'target_hp_pct',
                        command=lambda _v: self.select_metric()).pack(side='left', padx=(0, 8))
        self._ready_badge_frame = tk.Frame(controls, bg=_SAO_PANEL_BODY_BG)
        self._ready_badge_frame.pack(side='left', padx=(0, 8))
        status_badge(self._ready_badge_frame, 'READY', kind='ok').pack(side='left')
        action_button(controls, '刷新', self.refresh, kind='gold').pack(side='left', padx=(0, 6))
        action_button(controls, '导出 Export', self.export_json, kind='cyan').pack(side='left', padx=(0, 6))
        _make_panel_close_button(controls, self.hide, bg=_SAO_PANEL_BODY_BG, flat=True).pack(side='left', padx=(6, 0))

        # ── tag pills row ──
        pills = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        pills.pack(fill='x', padx=14, pady=(0, 8))
        self._pill_frame = pills

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=14, pady=(0, 14))
        self._rows = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG)
        self._rows.pack(fill='both', expand=True)
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        metric = str(status.get('selected_metric') or self._metric_var.get() or 'damage')
        series = status.get('series') if isinstance(status.get('series'), Mapping) else {}
        selected = series.get(metric) if isinstance(series.get(metric), Mapping) else {}
        points = _mapping_points(selected.get('points'))
        latest = points[-1].get('value') if points else 0
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}

        # ── update READY badge ──
        ok = bool(status.get('ok', True)) and not status.get('errors')
        badge_frame = getattr(self, '_ready_badge_frame', None)
        if badge_frame is not None:
            for child in list(badge_frame.winfo_children()):
                child.destroy()
            badge_text = 'READY' if ok else 'ERROR'
            badge_kind = 'ok' if ok else 'danger'
            status_badge(badge_frame, badge_text, kind=badge_kind).pack(side='left')

        # ── update tag pills ──
        pill_frame = getattr(self, '_pill_frame', None)
        if pill_frame is not None:
            for child in list(pill_frame.winfo_children()):
                child.destroy()
            encounter = status.get('encounter_id') or 'live'
            _sao_pill(pill_frame, f'采样 {len(points)} 点').pack(side='left', padx=(0, 6))
            if len(points) >= 2:
                t0 = _finite_int(points[0].get('time_ms'), 0, lo=0)
                t1 = _finite_int(points[-1].get('time_ms'), 0, lo=0)
                interval = (t1 - t0) / max(1, len(points) - 1) / 1000.0
                _sao_pill(pill_frame, f'间隔 {interval:.1f}s').pack(side='left', padx=(0, 6))
            _sao_pill(pill_frame, f'来源 {encounter}').pack(side='left', padx=(0, 6))

        if self._rows is None:
            return
        sig = self._series_signature(metric, points, status)
        if sig == self._last_series_sig:
            return
        self._last_series_sig = sig
        for child in list(self._rows.winfo_children()):
            child.destroy()
        if not points:
            empty_state(self._rows, '暂无 ACT 图表数据', '开始识别或 replay 后会出现曲线点。').pack(fill='both', expand=True, pady=8, padx=4)
            return
        self._render_chart(status, metric, points, latest)

    def _toggle_raw_points(self) -> None:
        self._last_series_sig = ""
        self._render_status(self._last_status)

    def _metric_defs(self, status: Mapping[str, Any]) -> list[Mapping[str, Any]]:
        metrics = status.get('metrics')
        if isinstance(metrics, list):
            valid = [item for item in metrics if isinstance(item, Mapping) and item.get('id')]
            if valid:
                return valid
        return list(_DEFAULT_METRICS)

    def _metric_label(self, status: Mapping[str, Any], metric: str) -> str:
        for item in self._metric_defs(status):
            if str(item.get('id') or '') == metric:
                return str(item.get('label') or metric)
        return metric

    def _render_chart(self, status: Mapping[str, Any], metric: str, points: list[Mapping[str, Any]], latest: Any) -> None:
        if self._rows is None:
            return
        series = status.get('series') if isinstance(status.get('series'), Mapping) else {}

        def _series(metric_id: str) -> list[Mapping[str, Any]]:
            entry = series.get(metric_id) if isinstance(series.get(metric_id), Mapping) else {}
            return _mapping_points(entry.get('points'))

        # Build lane definitions: (section_title, metric_id, points_list)
        lanes: list[tuple[str, str, list[Mapping[str, Any]]]] = [
            ('总伤害 Damage', 'damage', _series('damage') or points),
            ('治疗 Heal', 'heal', _series('heal')),
            ('事件数 Events', 'event_count', _series('event_count')),
        ]
        lanes = [lane for lane in lanes if lane[2]]
        if not lanes:
            lanes = [(self._metric_label(status, metric), metric, points)]

        for label, metric_id, lane_points in lanes:
            if not lane_points:
                continue
            lane_latest = _finite_float(lane_points[-1].get('value'), 0.0, lo=0.0) if lane_points else 0.0
            lane_peak = max((_finite_float(p.get('value'), 0.0, lo=0.0) for p in lane_points), default=0.0)
            subtitle = f"最新 {self._fmt(lane_latest)} · 峰值 {self._fmt(lane_peak)}"
            color = _BAR_COLORS.get(metric_id, _SAO_PANEL_ACCENT)
            accent = 'gold' if metric_id == 'damage' else ('ok' if metric_id == 'heal' else 'cyan')
            card = section_card(self._rows, f'▼ {label}', subtitle=subtitle, accent=accent)
            card.pack(fill='x', pady=(0, 8), padx=4)
            self._render_lane_canvas(card, metric_id, lane_points, color)

    def _render_lane_canvas(self, parent: tk.Misc, metric_id: str, lane_points: list[Mapping[str, Any]], color: str) -> None:
        """Draw a single bar-chart lane inside its section_card."""
        canvas = tk.Canvas(parent, height=140, bg=_SAO_PANEL_BODY_BG, bd=0, highlightthickness=0)
        canvas.pack(fill='x', expand=False, padx=4, pady=4)

        def _draw(_event: Any = None) -> None:
            w = max(1, canvas.winfo_width())
            h = max(1, canvas.winfo_height())
            canvas.delete('all')
            top = 12
            base = max(top + 20, h - 16)
            canvas.create_line(24, base, w - 16, base, fill='#274555', width=1)
            max_v = max(1.0, *[_finite_float(p.get('value'), 0.0, lo=0.0) for p in lane_points])
            max_bar_height = max(4, base - (top + 8))
            plot_l, plot_r = 24, max(40, w - 16)
            slot = (plot_r - plot_l) / max(1, len(lane_points))
            bar_w = max(3, min(16, int(slot * 0.84)))
            for idx, point in enumerate(lane_points):
                value = _finite_float(point.get('value'), 0.0, lo=0.0)
                height = max(4, int(max_bar_height * value / max_v))
                x = plot_l + idx * slot + max(0, (slot - bar_w) / 2)
                canvas.create_rectangle(x, base - height, x + bar_w, base, fill=color, outline=color)
            canvas.create_text(w - 18, top, text=self._fmt(max_v), fill=_SAO_PANEL_LABEL_FG,
                               font=get_cjk_font(8), anchor='ne')

        canvas.bind('<Configure>', _draw)
        canvas.after_idle(_draw)

    def _open_action_log_at(self, time_ms: int, topic: str) -> None:
        """Drill a graph point into the Action Log focused at that point's time."""
        owner = self.owner
        try:
            opener = getattr(owner, '_plugin_open_action_log_at', None)
            if not callable(opener):
                self._status_var.set('Action Log panel unavailable')
                return
            opener(_finite_int(time_ms, 0, lo=0), str(topic or ''))
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
        if not math.isfinite(number):
            number = 0.0
        if abs(number) >= 1_000_000:
            s = f"{number / 1_000_000:.2f}"
            s = s.rstrip('0').rstrip('.')
            return f"{s}m"
        if abs(number) >= 1_000:
            s = f"{number / 1_000:.1f}"
            s = s.rstrip('0').rstrip('.')
            return f"{s}k"
        if number == int(number):
            return str(int(number))
        return f"{number:.2f}"

    @staticmethod
    def _series_signature(metric: str, points: list[Mapping[str, Any]], status: Mapping[str, Any]) -> str:
        parts = []
        for point in points[-24:]:
            parts.append((point.get('time_ms'), point.get('topic'), point.get('value'), point.get('row_id')))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        return repr((
            metric,
            _finite_int(status.get('time_range_ms'), 0, lo=0),
            _finite_int(status.get('row_count'), len(points), lo=0),
            str(status.get('encounter_id') or ''),
            str(filters.get('query') or ''),
            str(filters.get('topic') or ''),
            tuple(str(err) for err in list(status.get('errors') or [])),
            parts,
        ))
