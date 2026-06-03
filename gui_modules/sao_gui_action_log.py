# -*- coding: utf-8 -*-
"""Entity-mode ACT action-log panel."""

from __future__ import annotations

import json
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_action_log_copy,
    act_action_log_filter,
    act_action_log_jump_to_time,
    act_action_log_search,
    act_action_log_status,
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


class ActionLogPanel:
    """SAO-styled compact searchable ACT action log for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="ACTION LOG: --")
        self._status_var = tk.StringVar(value="Ready")
        self._query_var = tk.StringVar(value="")
        self._topic_var = tk.StringVar(value="")
        self._cursor_var = tk.StringVar(value="0")
        self._source_var = tk.StringVar(value="live")
        self._encounter_var = tk.StringVar(value="")
        self._offset_var = tk.StringVar(value="0")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_rows_sig = ""

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
            cursor_ms = int(self._cursor_var.get() or 0)
        except Exception:
            cursor_ms = 0
        offset = self._current_offset()
        try:
            status = act_action_log_status(
                self.owner,
                limit=80,
                query=self._query_var.get(),
                topic=self._topic_var.get(),
                cursor_ms=cursor_ms,
                source=self._source_var.get(),
                encounter_id=self._encounter_var.get(),
                offset=offset,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "rows": [], "columns": [], "filters": {"query": self._query_var.get(), "topic": self._topic_var.get()}, "cursor": {"time_ms": cursor_ms, "offset": offset, "row_count": 0}, "analytics": {}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._render_status(self._last_status)
        return self._last_status

    def force_refresh(self) -> Dict[str, Any]:
        self._last_refresh_at = 0.0
        self._last_rows_sig = ""
        return self.refresh()

    def search(self) -> Dict[str, Any]:
        self._set_offset(0)
        return self._apply_result(act_action_log_search(
            self.owner,
            query=self._query_var.get(),
            limit=80,
            source=self._source_var.get(),
            encounter_id=self._encounter_var.get(),
            offset=0,
        ), 'SEARCH APPLIED')

    def filter_topic(self) -> Dict[str, Any]:
        self._set_offset(0)
        return self._apply_result(act_action_log_filter(
            self.owner,
            topic=self._topic_var.get(),
            query=self._query_var.get(),
            limit=80,
            source=self._source_var.get(),
            encounter_id=self._encounter_var.get(),
            offset=0,
        ), 'FILTER APPLIED')

    def jump_to_time(self) -> Dict[str, Any]:
        try:
            cursor_ms = int(self._cursor_var.get() or 0)
        except Exception:
            cursor_ms = 0
        return self._apply_result(act_action_log_jump_to_time(
            self.owner,
            cursor_ms=cursor_ms,
            limit=80,
            source=self._source_var.get(),
            encounter_id=self._encounter_var.get(),
            offset=self._current_offset(),
        ), f'JUMP {cursor_ms}ms')

    def copy_json(self) -> Dict[str, Any]:
        try:
            result = act_action_log_copy(
                self.owner,
                limit=80,
                query=self._query_var.get(),
                topic=self._topic_var.get(),
                source=self._source_var.get(),
                encounter_id=self._encounter_var.get(),
                offset=self._current_offset(),
            )
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "text": json.dumps(self._last_status, ensure_ascii=False, indent=2)}
        text = str(result.get('text') or json.dumps(self._last_status, ensure_ascii=False, indent=2))
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Action log copied to clipboard')
        except Exception as exc:
            self._status_var.set(str(exc))
            result = dict(result)
            result.update({"ok": False, "message": str(exc)})
        return dict(result or {})

    def previous_page(self) -> Dict[str, Any]:
        cursor = self._last_status.get('cursor') if isinstance(self._last_status.get('cursor'), Mapping) else {}
        limit = int(cursor.get('limit') or 80)
        self._set_offset(max(0, self._current_offset() - limit))
        return self.force_refresh()

    def next_page(self) -> Dict[str, Any]:
        cursor = self._last_status.get('cursor') if isinstance(self._last_status.get('cursor'), Mapping) else {}
        if cursor and cursor.get('has_next') is False:
            self._status_var.set('No next action-log page')
            return self._last_status
        limit = int(cursor.get('limit') or 80)
        self._set_offset(self._current_offset() + max(1, limit))
        return self.force_refresh()

    def _refresh_from_start(self) -> Dict[str, Any]:
        self._set_offset(0)
        return self.force_refresh()

    def _current_offset(self) -> int:
        try:
            return max(0, int(self._offset_var.get() or 0))
        except Exception:
            return 0

    def _set_offset(self, value: int) -> None:
        try:
            self._offset_var.set(str(max(0, int(value or 0))))
        except Exception:
            pass

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
        win.title('SAO ACT Action Log')
        win.geometry('900x560+230+165')
        win.minsize(720, 430)
        win.configure(bg=_SAO_PANEL_BG)
        try:
            win.overrideredirect(False)
            win.attributes('-alpha', 0.97)
        except Exception:
            pass
        try:
            _apply_window_icon(win)
        except Exception:
            pass
        header = _sao_panel_header(win, 'ACT ACTION LOG', on_close=self.hide)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'ACTION LOG').pack(side='left')
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(side='left', padx=(12, 0))
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('搜索 Search', self.search),
            ('复制 Copy', self.copy_json),
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
        tk.Label(control, text='Source', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._source_var, 'live', 'history', command=lambda _v: self._refresh_from_start()).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Search', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=18).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Topic', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._topic_var, '', 'damage', 'skill', 'boss', 'trigger', command=lambda _v: self.filter_topic()).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Encounter', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._encounter_var, width=14).pack(side='left', padx=(6, 8))
        tk.Label(control, text='Cursor ms', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._cursor_var, width=8).pack(side='left', padx=(6, 6))
        tk.Button(control, text='跳转 Jump', command=self.jump_to_time).pack(side='left', padx=(0, 8))
        tk.Button(control, text='过滤 Filter', command=self.filter_topic).pack(side='left', padx=(0, 8))
        tk.Button(control, text='上一页 Prev', command=self.previous_page).pack(side='left', padx=(0, 6))
        tk.Button(control, text='下一页 Next', command=self.next_page).pack(side='left')

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
        self._rows = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        rows = list(status.get('rows') or [])
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cursor = status.get('cursor') if isinstance(status.get('cursor'), Mapping) else {}
        source = str(status.get('source') or filters.get('source') or 'live')
        analytics = status.get('analytics') if isinstance(status.get('analytics'), Mapping) else {}
        page = analytics.get('page') if isinstance(analytics.get('page'), Mapping) else {}
        total_rows = int(analytics.get('total_rows') or cursor.get('total_row_count') or len(rows))
        page_index = int(page.get('page_index') or cursor.get('page_index') or 0)
        page_count = int(page.get('page_count') or cursor.get('page_count') or 0)
        self._set_offset(int(page.get('offset') or cursor.get('offset') or self._current_offset()))
        self._summary_var.set(
            f"{len(rows)}/{total_rows} ROWS · {source.upper()} · PAGE {page_index}/{page_count} · {filters.get('topic') or 'ALL'}"
        )
        errors = list(status.get('errors') or [])
        self._status_var.set(
            f"encounter={status.get('encounter_id') or filters.get('encounter_id') or source} · cursor={int(cursor.get('time_ms') or 0)}ms · query={filters.get('query') or '-'} · errors={len(errors)}"
        )
        if self._rows is None:
            return
        sig = self._rows_signature(rows)
        if sig == self._last_rows_sig:
            return
        self._last_rows_sig = sig
        for child in list(self._rows.winfo_children()):
            child.destroy()
        if not rows:
            self._render_empty()
            return
        self._render_header()
        for row in rows[:80]:
            self._render_row(row)

    def _render_header(self) -> None:
        if self._rows is None:
            return
        header = tk.Frame(self._rows, bg=_SAO_PANEL_HEADER_BG)
        header.pack(fill='x', pady=(0, 2))
        for text, width in (('Time', 10), ('Topic', 12), ('Action', 34), ('Value', 14), ('Source', 16)):
            tk.Label(header, text=text, width=width, anchor='w', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, font=('Segoe UI', 9, 'bold')).pack(side='left', padx=3, pady=5)

    def _render_empty(self) -> None:
        if self._rows is None:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(
            box,
            text='暂无 ACT 行为日志\nlive 模式等待事件；history 模式需要 SQLite actions。',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=('Segoe UI', 10),
            pady=28,
        ).pack(fill='x')

    def _render_row(self, row: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        bg = '#2b2a1a' if row.get('is_cursor') else _SAO_PANEL_BODY_BG
        card = tk.Frame(self._rows, bg=bg, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        card.pack(fill='x', pady=3, padx=4)
        top = tk.Frame(card, bg=bg)
        top.pack(fill='x', padx=8, pady=(6, 2))
        values = (
            (f"{int(row.get('time_ms') or 0)}ms", 10, _SAO_PANEL_LABEL_FG),
            (str(row.get('topic') or '-'), 12, _SAO_PANEL_GOLD),
            (str(row.get('label') or '-'), 34, _SAO_PANEL_VALUE_FG),
            (str(row.get('value') or ''), 14, _SAO_PANEL_VALUE_FG),
            (str(row.get('source') or '-'), 16, _SAO_PANEL_LABEL_FG),
        )
        for text, width, fg in values:
            tk.Label(top, text=text, width=width, anchor='w', bg=bg, fg=fg, font=('Segoe UI', 9)).pack(side='left', padx=3)
        meta = f"actor={row.get('actor') or '-'} · target={row.get('target') or '-'}"
        tk.Label(card, text=meta, bg=bg, fg=_SAO_PANEL_LABEL_FG, anchor='w', font=('Segoe UI', 8)).pack(fill='x', padx=8, pady=(0, 6))

    @staticmethod
    def _rows_signature(rows: list[Any]) -> str:
        parts = []
        for row in rows[:80]:
            if not isinstance(row, Mapping):
                continue
            parts.append((row.get('id'), row.get('time_ms'), row.get('topic'), row.get('label'), row.get('value'), bool(row.get('is_cursor'))))
        return repr(parts)
