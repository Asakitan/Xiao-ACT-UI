# -*- coding: utf-8 -*-
"""Entity-mode ACT action-log panel."""

from __future__ import annotations

import json
import math
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
from gui_modules.sao_panel_components import (
    SP_XS,
    SP_LG,
    action_button,
    aggregate_row,
    attach_tooltip,
    detail_row,
    empty_state,
    fmt_clock,
    fmt_dur,
    keep_canvas_scroll,
    metric_tile,
    readable_event_line,
    section_card,
    status_badge,
    topic_cn,
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
    _theme_color,
)


def _finite_float(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> float:
    try:
        number = float(default if value is None or value == '' else value)
    except Exception:
        number = float(default or 0.0)
    if not math.isfinite(number):
        number = float(default or 0.0)
    if lo is not None:
        number = max(float(lo), number)
    if hi is not None:
        number = min(float(hi), number)
    return number


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    number = int(_finite_float(value, float(default), lo=lo, hi=hi))
    if lo is not None:
        number = max(int(lo), number)
    if hi is not None:
        number = min(int(hi), number)
    return number


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
        self._last_request_key: tuple[Any, ...] = ()
        self._last_rows_sig = ""
        self._expanded_groups: set[str] = set()
        self._show_raw_rows = tk.BooleanVar(value=False)

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
        cursor_ms = _finite_int(self._cursor_var.get(), 0, lo=0)
        offset = self._current_offset()
        query = self._query_var.get()
        topic = self._topic_var.get()
        source = self._source_var.get()
        encounter_id = self._encounter_var.get()
        request_key = (query, topic, cursor_ms, source, encounter_id, offset)
        if (
            self._last_status
            and request_key == self._last_request_key
            and now - self._last_refresh_at < 0.35
        ):
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_action_log_status(
                self.owner,
                limit=80,
                query=query,
                topic=topic,
                cursor_ms=cursor_ms,
                source=source,
                encounter_id=encounter_id,
                offset=offset,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "rows": [], "columns": [], "filters": {"query": query, "topic": topic, "source": source, "encounter_id": encounter_id}, "cursor": {"time_ms": cursor_ms, "offset": offset, "row_count": 0}, "analytics": {}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def force_refresh(self) -> Dict[str, Any]:
        self._last_refresh_at = 0.0
        self._last_request_key = ()
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
        cursor_ms = _finite_int(self._cursor_var.get(), 0, lo=0)
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
        limit = _finite_int(cursor.get('limit'), 80, lo=1)
        self._set_offset(max(0, self._current_offset() - limit))
        return self.force_refresh()

    def next_page(self) -> Dict[str, Any]:
        cursor = self._last_status.get('cursor') if isinstance(self._last_status.get('cursor'), Mapping) else {}
        if cursor and cursor.get('has_next') is False:
            self._status_var.set('No next action-log page')
            return self._last_status
        limit = _finite_int(cursor.get('limit'), 80, lo=1)
        self._set_offset(self._current_offset() + max(1, limit))
        return self.force_refresh()

    def _refresh_from_start(self) -> Dict[str, Any]:
        self._set_offset(0)
        return self.force_refresh()

    def _current_offset(self) -> int:
        try:
            return _finite_int(self._offset_var.get(), 0, lo=0)
        except Exception:
            return 0

    def _set_offset(self, value: int) -> None:
        try:
            self._offset_var.set(str(_finite_int(value, 0, lo=0)))
        except Exception:
            pass

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
        win.title('SAO ACT Action Log')
        win.geometry('900x560+230+165')
        win.minsize(720, 430)
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
        header = _sao_panel_header(win, 'ACT ACTION LOG', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
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
            action_button(toolbar, label, cmd, kind='cyan' if '复制' in label else 'gold').pack(side='right', padx=(6, 0))

        # 筛选行：来源 / 搜索 / 类型 / 战斗 ID + 过滤（原 10+ 控件单行拆成两行分组）
        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(control, text='来源', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._source_var, 'live', 'history', command=lambda _v: self._refresh_from_start()).pack(side='left', padx=(6, 8))
        tk.Label(control, text='搜索', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.Entry(control, textvariable=self._query_var, width=18).pack(side='left', padx=(6, 8))
        tk.Label(control, text='类型', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9)).pack(side='left')
        tk.OptionMenu(control, self._topic_var, '', 'damage', 'skill', 'boss', 'trigger', command=lambda _v: self.filter_topic()).pack(side='left', padx=(6, 8))
        enc_label = tk.Label(control, text='战斗 ID', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9))
        enc_label.pack(side='left')
        enc_entry = tk.Entry(control, textvariable=self._encounter_var, width=14)
        enc_entry.pack(side='left', padx=(6, 8))
        for w in (enc_label, enc_entry):
            attach_tooltip(w, '按战斗 (encounter) ID 过滤日志；留空显示全部')
        action_button(control, '过滤 Filter', self.filter_topic, kind='cyan').pack(side='left', padx=(0, 8))

        # 导航行：游标跳转 / 翻页 / RAW 行模式
        nav = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        nav.pack(fill='x', padx=12, pady=(0, 8))
        cursor_label = tk.Label(nav, text='游标 ms', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 9))
        cursor_label.pack(side='left')
        cursor_entry = tk.Entry(nav, textvariable=self._cursor_var, width=10)
        cursor_entry.pack(side='left', padx=(6, 6))
        for w in (cursor_label, cursor_entry):
            attach_tooltip(w, '跳转到该时间点 (epoch 毫秒)；跳转后日志定位到此游标')
        action_button(nav, '跳转 Jump', self.jump_to_time, kind='gold').pack(side='left', padx=(0, 12))
        action_button(nav, '上一页 Prev', self.previous_page).pack(side='left', padx=(0, 6))
        action_button(nav, '下一页 Next', self.next_page).pack(side='left')
        raw_check = tk.Checkbutton(
            nav,
            text='RAW 行',
            variable=self._show_raw_rows,
            command=self._toggle_raw_rows,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            selectcolor=_SAO_PANEL_HEADER_BG,
            activebackground=_SAO_PANEL_BODY_BG,
            activeforeground=_SAO_PANEL_GOLD,
            font=('Segoe UI', 9),
        )
        raw_check.pack(side='left', padx=(12, 0))
        attach_tooltip(raw_check, '显示未聚合的原始事件行（默认按动作聚合展示）')

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
        _win_id = canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        rows = list(status.get('rows') or [])
        groups = list(status.get('grouped_rows') or [])
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cursor = status.get('cursor') if isinstance(status.get('cursor'), Mapping) else {}
        source = str(status.get('source') or filters.get('source') or 'live')
        analytics = status.get('analytics') if isinstance(status.get('analytics'), Mapping) else {}
        page = analytics.get('page') if isinstance(analytics.get('page'), Mapping) else {}
        total_rows = _finite_int(analytics.get('total_rows') or cursor.get('total_row_count'), len(rows), lo=0)
        page_index = _finite_int(page.get('page_index') or cursor.get('page_index'), 0, lo=0)
        page_count = _finite_int(page.get('page_count') or cursor.get('page_count'), 0, lo=0)
        self._set_offset(_finite_int(page.get('offset') or cursor.get('offset'), self._current_offset(), lo=0))
        self._summary_var.set(
            f"{len(rows)}/{total_rows} ROWS · {source.upper()} · PAGE {page_index}/{page_count} · {filters.get('topic') or 'ALL'}"
        )
        errors = list(status.get('errors') or [])
        cursor_ms = _finite_int(cursor.get('time_ms'), 0, lo=0)
        self._status_var.set(
            f"encounter={status.get('encounter_id') or filters.get('encounter_id') or source} · cursor={fmt_clock(cursor_ms) if cursor_ms else '--'} · query={filters.get('query') or '-'} · errors={len(errors)}"
        )
        if self._rows is None:
            return
        totals = analytics.get('totals') if isinstance(analytics.get('totals'), Mapping) else {}
        group_info = analytics.get('groups') if isinstance(analytics.get('groups'), Mapping) else {}
        topic_groups = list(group_info.get('topics') or []) if isinstance(group_info.get('topics'), list) else []
        top_topic = topic_groups[0].get('key') if topic_groups and isinstance(topic_groups[0], Mapping) else '-'
        sig = repr((
            self._rows_signature(rows),
            self._groups_signature(groups, self._expanded_groups),
            sorted(self._expanded_groups),
            bool(self._show_raw_rows.get()),
            source,
            status.get('encounter_id') or filters.get('encounter_id'),
            filters.get('topic'),
            filters.get('query'),
            cursor_ms,
            total_rows,
            page_index,
            page_count,
            self._current_offset(),
            totals.get('value'),
            top_topic,
            tuple(errors),
        ))
        if sig == self._last_rows_sig:
            return
        self._last_rows_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_metrics(status, rows, groups, analytics, cursor, source)
        if not rows:
            empty_state(self._rows, '暂无 ACT 行为日志', 'live 模式等待事件；history 模式需要 SQLite actions。').pack(fill='x', pady=8, padx=4)
            return
        if groups and not self._show_raw_rows.get():
            self._render_group_section(groups)
            return
        if groups:
            self._render_group_section(groups, title='行为聚合摘要', subtitle='RAW 模式已开启，下方同时显示原始行。')
        self._render_raw_section(rows)

    def _toggle_raw_rows(self) -> None:
        self._last_rows_sig = ""
        self._render_status(self._last_status)

    def _render_metrics(self, status: Mapping[str, Any], rows: list[Any], groups: list[Any], analytics: Mapping[str, Any], cursor: Mapping[str, Any], source: str) -> None:
        if self._rows is None:
            return
        totals = analytics.get('totals') if isinstance(analytics.get('totals'), Mapping) else {}
        page = analytics.get('page') if isinstance(analytics.get('page'), Mapping) else {}
        group_info = analytics.get('groups') if isinstance(analytics.get('groups'), Mapping) else {}
        topic_groups = list(group_info.get('topics') or []) if isinstance(group_info.get('topics'), list) else []
        top_topic = topic_groups[0].get('key') if topic_groups and isinstance(topic_groups[0], Mapping) else '-'
        total_rows = _finite_int(analytics.get('total_rows') or cursor.get('total_row_count'), len(rows), lo=0)
        page_index = _finite_int(page.get('page_index') or cursor.get('page_index'), 0, lo=0)
        page_count = _finite_int(page.get('page_count') or cursor.get('page_count'), 0, lo=0)
        page_offset = _finite_int(page.get('offset') or cursor.get('offset'), 0, lo=0)
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        items = (
            ('Rows', f"{len(rows)}/{total_rows}", source.upper(), 'cyan'),
            ('Value', self._fmt(totals.get('value')), 'all filtered rows', 'gold'),
            ('Groups', len(groups), f"top {top_topic}", 'gold'),
            ('Page', f"{page_index}/{page_count}", f"offset {page_offset}", 'cyan'),
        )
        for label, value, sub, accent in items:
            metric_tile(grid, label, value, sub=str(sub), accent=accent).pack(side='left', fill='x', expand=True, padx=3)
        badges = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        badges.pack(fill='x', padx=4, pady=(0, 8))
        status_badge(badges, f"模式 {'实时' if source == 'live' else ('历史' if source == 'history' else source)}", kind='cyan').pack(side='left', padx=(0, 6))
        status_badge(badges, f"RAW {'ON' if self._show_raw_rows.get() else 'OFF'}", kind='gold').pack(side='left', padx=(0, 6))
        status_badge(badges, f"ERRORS {len(status.get('errors') or [])}", kind='danger' if status.get('errors') else 'cyan').pack(side='left', padx=(0, 6))

    def _render_group_section(self, groups: list[Any], *, title: str = '行为日志聚合', subtitle: str = '默认按动作/技能/怪物/地牢语义分组；展开后才看 raw samples。') -> None:
        if self._rows is None:
            return
        box = section_card(self._rows, title, subtitle=subtitle, badge=str(len(groups)))
        box.pack(fill='x', padx=4, pady=(4, 9))
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=8, pady=8)
        valid = [group for group in groups if isinstance(group, Mapping)]
        if not valid:
            empty_state(body, '暂无聚合分组', '当前筛选条件没有可折叠的语义分组。').pack(fill='x')
            return
        max_value = max(1.0, *[_finite_float(group.get('total_value'), 0.0, lo=0.0) for group in valid])
        for idx, group in enumerate(valid[:80]):
            key = str(group.get('key') or idx)
            open_group = key in self._expanded_groups
            value = _finite_float(group.get('total_value'), 0.0, lo=0.0)
            first_ms = _finite_int(group.get('first_time_ms'), 0, lo=0)
            last_ms = _finite_int(group.get('last_time_ms'), first_ms, lo=0)
            count = _finite_int(group.get('count'), 0, lo=0)
            uid_count = _finite_int(group.get('uid_count'), 0, lo=0)
            meta = (
                f"{topic_cn(group.get('kind'))} · {count} 条 · "
                f"{uid_count} 个UID · {fmt_clock(first_ms)} · {fmt_dur(last_ms - first_ms)}"
            )
            if group.get('dungeons'):
                meta += f" · {'/'.join(str(x) for x in list(group.get('dungeons') or [])[:3])}"
            aggregate_row(
                body,
                title=str(group.get('name') or '-'),
                meta=meta,
                value=f"{self._fmt(value)} · {count}x",
                ratio=value / max_value if max_value else 0.0,
                accent=('cyan' if str(group.get('kind') or '') == 'system' else ('danger' if str(group.get('kind') or '') in {'damage', 'monster', 'target'} else 'gold')),
                zebra=bool(idx % 2),
                command=lambda k=key: self._toggle_group(k),
                expanded=open_group,
            ).pack(fill='x', pady=2)
            if open_group:
                self._render_group_details(body, group)

    def _render_group_details(self, parent: tk.Misc, group: Mapping[str, Any]) -> None:
        """Representative rows under an expanded group (was a missing method that
        crashed every group expand — the user's '点开进不去')."""
        accent = 'danger' if str(group.get('kind') or '') in {'damage', 'monster', 'target'} else 'gold'
        rows = [row for row in list(group.get('rows') or []) if isinstance(row, Mapping)][:8]
        for ridx, row in enumerate(rows):
            detail_row(parent, readable_event_line(row, value_fmt=self._fmt), accent=accent, zebra=bool(ridx % 2)).pack(fill='x', padx=(SP_LG, SP_XS), pady=1)
        if group.get('has_more_rows'):
            detail_row(parent, '还有更多明细，请缩小筛选或翻页查看。', accent=accent).pack(fill='x', padx=(SP_LG, SP_XS), pady=1)

    def _render_raw_section(self, rows: list[Any]) -> None:
        if self._rows is None:
            return
        box = section_card(self._rows, 'RAW 行明细', subtitle='仅用于排查和复制；默认不作为主要阅读视图。', badge=str(len(rows)))
        box.pack(fill='x', padx=4, pady=(4, 9))
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=8, pady=8)
        self._render_header(body)
        for row in rows[:80]:
            self._render_row(row, parent=body)

    def _toggle_group(self, key: str) -> None:
        key = str(key or '')
        if not key:
            return
        if key in self._expanded_groups:
            self._expanded_groups.remove(key)
        else:
            self._expanded_groups.add(key)
        self._last_rows_sig = ""
        self._render_status(self._last_status)

    def _render_group(self, group: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        key = str(group.get('key') or '')
        open_group = key in self._expanded_groups
        card = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        card.pack(fill='x', pady=4, padx=4)
        top = tk.Frame(card, bg=_SAO_PANEL_HEADER_BG)
        top.pack(fill='x')
        toggle = '▾' if open_group else '▸'
        title = f"{toggle} {group.get('name') or '-'}"
        count = _finite_int(group.get('count'), 0, lo=0)
        uid_count = _finite_int(group.get('uid_count'), 0, lo=0)
        first_ms = _finite_int(group.get('first_time_ms'), 0, lo=0)
        last_ms = _finite_int(group.get('last_time_ms'), first_ms, lo=0)
        meta = (
            f"{str(group.get('kind') or 'event').upper()} · {count} rows · "
            f"UID {uid_count} · {first_ms}-{last_ms}ms"
        )
        tk.Button(
            top,
            text=title,
            command=lambda k=key: self._toggle_group(k),
            anchor='w',
            bg=_SAO_PANEL_HEADER_BG,
            fg=_SAO_PANEL_VALUE_FG,
            activebackground=_SAO_PANEL_ACCENT,
            activeforeground='white',
            relief='flat',
            bd=0,
            font=('Segoe UI', 9, 'bold'),
        ).pack(side='left', fill='x', expand=True, padx=(8, 4), pady=6)
        tk.Label(
            top,
            text=str(group.get('total_value') or 0),
            bg=_SAO_PANEL_HEADER_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 9, 'bold'),
        ).pack(side='right', padx=8)
        tk.Label(card, text=meta, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, anchor='w', font=('Segoe UI', 8)).pack(fill='x', padx=8, pady=(4, 6))
        if not open_group:
            return
        for row in list(group.get('rows') or [])[:80]:
            if isinstance(row, Mapping):
                self._render_group_detail(card, row)
        if group.get('has_more_rows'):
            tk.Label(card, text='还有更多明细，请缩小筛选或翻页查看。', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, anchor='w', font=('Segoe UI', 8)).pack(fill='x', padx=14, pady=(0, 8))

    def _render_group_detail(self, parent: tk.Misc, row: Mapping[str, Any]) -> None:
        payload = row.get('payload') if isinstance(row.get('payload'), Mapping) else {}
        uid = row.get('target_uid') or payload.get('target_uuid') or payload.get('target_uid') or row.get('actor_uid') or payload.get('actor_uid') or '-'
        dungeon = row.get('dungeon') or payload.get('dungeon_name') or payload.get('dungeon_id') or '-'
        # 明细盒用主题常量 — 旧硬编码 '#081521' 近黑底是扁平化前残留,
        # 浅色 LABEL/VALUE_FG 灰字打上去对比度严重不足
        box = tk.Frame(parent, bg=_SAO_PANEL_HEADER_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=3, padx=12)
        top = tk.Frame(box, bg=_SAO_PANEL_HEADER_BG)
        top.pack(fill='x', padx=8, pady=(5, 2))
        for text, width, fg in (
            (f"{_finite_int(row.get('time_ms'), 0, lo=0)}ms", 10, _SAO_PANEL_LABEL_FG),
            (str(row.get('topic') or '-'), 12, _SAO_PANEL_GOLD),
            (str(row.get('label') or '-'), 34, _SAO_PANEL_VALUE_FG),
            (str(row.get('value') or ''), 14, _SAO_PANEL_VALUE_FG),
        ):
            tk.Label(top, text=text, width=width, anchor='w', bg=_SAO_PANEL_HEADER_BG, fg=fg, font=('Segoe UI', 8)).pack(side='left', padx=2)
        meta = f"actor={row.get('actor') or '-'} · target={row.get('target') or '-'} · uid={uid} · dungeon={dungeon} · source={row.get('source') or '-'}"
        tk.Label(box, text=meta, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_LABEL_FG, anchor='w', font=('Segoe UI', 8)).pack(fill='x', padx=8, pady=(0, 5))

    def _render_header(self, parent: Optional[tk.Misc] = None) -> None:
        parent = parent or self._rows
        if parent is None:
            return
        header = tk.Frame(parent, bg=_SAO_PANEL_HEADER_BG)
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

    def _render_row(self, row: Mapping[str, Any], parent: Optional[tk.Misc] = None) -> None:
        parent = parent or self._rows
        if parent is None:
            return
        # 游标行用主题 warn_soft 淡金底（旧 '#2b2a1a' 深橄榄色在浅色主题下是黑块）
        bg = _theme_color('warn_soft', '#fff8e5') if row.get('is_cursor') else _SAO_PANEL_BODY_BG
        card = tk.Frame(parent, bg=bg, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        card.pack(fill='x', pady=3, padx=4)
        top = tk.Frame(card, bg=bg)
        top.pack(fill='x', padx=8, pady=(6, 2))
        values = (
            (fmt_clock(row.get('time_ms')), 10, _SAO_PANEL_LABEL_FG),
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
            parts.append((
                row.get('id'),
                row.get('time_ms'),
                row.get('topic'),
                row.get('source'),
                row.get('label'),
                row.get('value'),
                row.get('actor'),
                row.get('target'),
                bool(row.get('is_cursor')),
            ))
        return repr((len(rows), parts))

    @staticmethod
    def _groups_signature(groups: list[Any], expanded_groups: set[str]) -> str:
        expanded = {str(key) for key in expanded_groups}
        parts = []
        for idx, group in enumerate(groups[:80]):
            if not isinstance(group, Mapping):
                continue
            key = str(group.get('key') or idx)
            row_parts = []
            if key in expanded:
                for row in list(group.get('rows') or [])[:8]:
                    if not isinstance(row, Mapping):
                        continue
                    payload = row.get('payload') if isinstance(row.get('payload'), Mapping) else None
                    row_parts.append((
                        row.get('id'),
                        row.get('time_ms'),
                        row.get('topic'),
                        row.get('source'),
                        row.get('actor'),
                        row.get('actor_uid'),
                        row.get('target'),
                        row.get('target_uid'),
                        row.get('label'),
                        row.get('value'),
                        row.get('dungeon'),
                        payload,
                    ))
            parts.append((
                key,
                group.get('name'),
                group.get('kind'),
                group.get('count'),
                group.get('total_value'),
                group.get('uid_count'),
                group.get('first_time_ms'),
                group.get('last_time_ms'),
                tuple(group.get('dungeons') or ()),
                bool(group.get('has_more_rows')),
                tuple(row_parts),
            ))
        return repr((len(groups), parts))

    @staticmethod
    def _fmt(value: Any) -> str:
        try:
            number = float(value or 0.0)
        except Exception:
            return str(value or '0')
        if not math.isfinite(number):
            number = 0.0
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        return str(int(number)) if number == int(number) else f"{number:.2f}"
