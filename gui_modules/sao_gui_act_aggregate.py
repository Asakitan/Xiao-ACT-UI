# -*- coding: utf-8 -*-
"""Entity-mode ACT aggregate cockpit panel."""

from __future__ import annotations

import json
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_action_log_status,
    act_graph_timeseries_status,
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


class ActAggregatePanel:
    """Single ACT overview for summary, groups, and graph preview."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="ACT AGGREGATE: --")
        self._status_var = tk.StringVar(value="Ready")
        self._query_var = tk.StringVar(value="")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_sig = ""
        self._expanded_groups: set[str] = set()

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
        snapshot = self._snapshot()
        query = self._query_var.get()
        try:
            action_log = act_action_log_status(self.owner, limit=80, query=query)
        except Exception as exc:
            action_log = {"ok": False, "message": str(exc), "rows": [], "grouped_rows": [], "analytics": {}, "errors": [str(exc)]}
        try:
            graph = act_graph_timeseries_status(self.owner, limit=120, query=query)
        except Exception as exc:
            graph = {"ok": False, "message": str(exc), "series": {}, "row_count": 0, "errors": [str(exc)]}
        status = {
            "ok": bool(action_log.get("ok", True)) and bool(graph.get("ok", True)),
            "snapshot": snapshot,
            "action_log": action_log,
            "graph": graph,
            "query": query,
            "errors": list(action_log.get("errors") or []) + list(graph.get("errors") or []),
        }
        self._last_status = status
        self._last_refresh_at = now
        self._render_status(status)
        return status

    def filter(self) -> Dict[str, Any]:
        self._last_refresh_at = 0.0
        self._last_sig = ""
        return self.refresh()

    def copy_json(self) -> Dict[str, Any]:
        text = json.dumps(self._last_status or self.refresh(), ensure_ascii=False, indent=2, default=str)
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Aggregate JSON copied to clipboard')
            return {"ok": True, "text": text}
        except Exception as exc:
            self._status_var.set(str(exc))
            return {"ok": False, "message": str(exc), "text": text}

    def _snapshot(self) -> Dict[str, Any]:
        for name in ("_get_dps_act_snapshot", "_build_dps_act_snapshot"):
            fn = getattr(self.owner, name, None)
            if callable(fn):
                try:
                    snap = fn(history_limit=20)
                except TypeError:
                    try:
                        snap = fn()
                    except Exception:
                        snap = {}
                except Exception:
                    snap = {}
                if isinstance(snap, Mapping):
                    return dict(snap)
        return {}

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Aggregate')
        win.geometry('960x620+210+130')
        win.minsize(760, 460)
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
        header = _sao_panel_header(win, 'ACT AGGREGATE COCKPIT', on_close=self.hide)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'AGGREGATE').pack(side='left')
        tk.Label(toolbar, textvariable=self._summary_var, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 10, 'bold')).pack(side='left', padx=(12, 0))
        for label, cmd in (('刷新 Refresh', self.refresh), ('复制 Copy', self.copy_json), ('关闭 Close', self.hide)):
            tk.Button(toolbar, text=label, command=cmd, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_HEADER_FG, activebackground=_SAO_PANEL_ACCENT, activeforeground='white', relief='flat', bd=0, padx=10, pady=4).pack(side='right', padx=(6, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='Search', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=28).pack(side='left', padx=(6, 8))
        tk.Button(control, text='过滤 Filter', command=self.filter).pack(side='left')
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
        snap = status.get('snapshot') if isinstance(status.get('snapshot'), Mapping) else {}
        spec = snap.get('render_spec') if isinstance(snap.get('render_spec'), Mapping) else {}
        totals = spec.get('totals') if isinstance(spec.get('totals'), Mapping) else {}
        rows = list(spec.get('rows') or []) if isinstance(spec.get('rows'), list) else []
        action_log = status.get('action_log') if isinstance(status.get('action_log'), Mapping) else {}
        graph = status.get('graph') if isinstance(status.get('graph'), Mapping) else {}
        grouped = list(action_log.get('grouped_rows') or [])
        self._summary_var.set(f"DMG {self._fmt(totals.get('damage'))} · HEAL {self._fmt(totals.get('heal'))} · MEMBERS {len(rows)} · GROUPS {len(grouped)}")
        self._status_var.set(f"encounter={snap.get('encounter_id') or action_log.get('encounter_id') or 'live'} · query={status.get('query') or '-'} · graphRows={graph.get('row_count') or 0} · errors={len(status.get('errors') or [])}")
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_cards(totals, rows)
        self._render_member_rows(rows)
        self._render_groups(grouped)
        self._render_graph_preview(graph)

    def _render_cards(self, totals: Mapping[str, Any], rows: list[Any]) -> None:
        if self._rows is None:
            return
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        items = (
            ('Damage', self._fmt(totals.get('damage'))),
            ('DPS', self._fmt(totals.get('dps'))),
            ('Heal', self._fmt(totals.get('heal'))),
            ('HPS', self._fmt(totals.get('hps'))),
            ('Elapsed', f"{int(float(totals.get('elapsed_s') or 0))}s"),
            ('Members', str(len(rows))),
        )
        for label, value in items:
            card = tk.Frame(grid, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            card.pack(side='left', fill='x', expand=True, padx=2)
            tk.Label(card, text=label, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8)).pack(anchor='w', padx=6, pady=(5, 0))
            tk.Label(card, text=str(value), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Segoe UI', 10, 'bold')).pack(anchor='w', padx=6, pady=(1, 5))

    def _render_member_rows(self, rows: list[Any]) -> None:
        if self._rows is None:
            return
        box = self._section('TOP COMBATANTS / AGGREGATED DAMAGE')
        if not rows:
            tk.Label(box, text='No combatant rows yet', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, pady=12).pack(fill='x')
            return
        max_amount = max(1.0, *[float((row or {}).get('damage') or 0) for row in rows if isinstance(row, Mapping)])
        for row in [item for item in rows if isinstance(item, Mapping)][:12]:
            line = tk.Frame(box, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            line.pack(fill='x', padx=8, pady=2)
            amount = float(row.get('damage') or 0)
            width = max(4, min(260, int(260 * amount / max_amount)))
            tk.Frame(line, bg=_SAO_PANEL_GOLD, width=width, height=3).pack(fill='x', anchor='w')
            tk.Label(line, text=str(row.get('name') or row.get('uid') or 'Unknown'), width=28, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Segoe UI', 9)).pack(side='left', padx=6, pady=4)
            tk.Label(line, text=f"DMG {self._fmt(row.get('damage'))}", width=16, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold')).pack(side='left', padx=6, pady=4)
            tk.Label(line, text=f"DPS {self._fmt(row.get('dps'))}", width=16, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left', padx=6, pady=4)

    def _render_groups(self, groups: list[Any]) -> None:
        if self._rows is None:
            return
        box = self._section('ACTION / SKILL STRUCTURE GROUPS')
        if not groups:
            tk.Label(box, text='No grouped ACT rows yet', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, pady=12).pack(fill='x')
            return
        for idx, group in enumerate([item for item in groups if isinstance(item, Mapping)][:16]):
            gid = str(group.get('id') or group.get('key') or idx)
            header = tk.Frame(box, bg=_SAO_PANEL_HEADER_BG, cursor='hand2')
            header.pack(fill='x', padx=8, pady=(4, 0))
            title = f"{group.get('label') or group.get('key') or 'GROUP'} · {int(group.get('count') or 0)} · {self._fmt(group.get('total_value'))}"
            label = tk.Label(header, text=('▼ ' if gid in self._expanded_groups else '▶ ') + title, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, anchor='w', font=('Segoe UI', 9, 'bold'), cursor='hand2')
            label.pack(fill='x', padx=6, pady=4)
            header.bind('<Button-1>', lambda _e, key=gid: self._toggle_group(key))
            label.bind('<Button-1>', lambda _e, key=gid: self._toggle_group(key))
            if gid in self._expanded_groups:
                for row in list(group.get('rows') or [])[:8]:
                    if not isinstance(row, Mapping):
                        continue
                    tk.Label(box, text=f"  {int(row.get('time_ms') or 0)}ms  {row.get('topic') or '-'}  {row.get('label') or '-'}  {row.get('value') or ''}", bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, anchor='w', font=('Segoe UI', 9)).pack(fill='x', padx=18, pady=1)

    def _render_graph_preview(self, graph: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        box = self._section('GRAPH / TIMESERIES PREVIEW')
        series = graph.get('series') if isinstance(graph.get('series'), Mapping) else {}
        for metric in ('damage', 'heal', 'event_count'):
            item = series.get(metric) if isinstance(series.get(metric), Mapping) else {}
            points = list(item.get('points') or [])
            latest = points[-1].get('value') if points else 0
            tk.Label(box, text=f"{metric.upper()}: {len(points)} pts · latest {self._fmt(latest)}", bg=_SAO_PANEL_BODY_BG, fg=(_SAO_PANEL_GOLD if metric == 'damage' else _SAO_PANEL_LABEL_FG), anchor='w', font=('Segoe UI', 9)).pack(fill='x', padx=8, pady=2)

    def _section(self, title: str) -> tk.Frame:
        assert self._rows is not None
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', padx=4, pady=(8, 0))
        tk.Label(box, text=title, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold'), anchor='w').pack(fill='x')
        return box

    def _toggle_group(self, group_id: str) -> None:
        if group_id in self._expanded_groups:
            self._expanded_groups.remove(group_id)
        else:
            self._expanded_groups.add(group_id)
        self._last_sig = ""
        self._render_status(self._last_status)

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

    def _signature(self, status: Mapping[str, Any]) -> str:
        snap = status.get('snapshot') if isinstance(status.get('snapshot'), Mapping) else {}
        spec = snap.get('render_spec') if isinstance(snap.get('render_spec'), Mapping) else {}
        action_log = status.get('action_log') if isinstance(status.get('action_log'), Mapping) else {}
        graph = status.get('graph') if isinstance(status.get('graph'), Mapping) else {}
        return repr((
            spec.get('totals'),
            [(row.get('uid'), row.get('damage'), row.get('heal')) for row in list(spec.get('rows') or [])[:16] if isinstance(row, Mapping)],
            [(group.get('id'), group.get('key'), group.get('count'), group.get('total_value')) for group in list(action_log.get('grouped_rows') or [])[:16] if isinstance(group, Mapping)],
            graph.get('row_count'),
            tuple(sorted(self._expanded_groups)),
            status.get('query'),
        ))
