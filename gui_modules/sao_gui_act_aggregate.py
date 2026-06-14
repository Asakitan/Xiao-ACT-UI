# -*- coding: utf-8 -*-
"""Entity-mode ACT semantic aggregate cockpit panel."""

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_aggregate_status,
    act_graph_timeseries_status,
    act_render_apply_hooks,
)
from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules.sao_panel_components import (
    SP_XS,
    SP_SM,
    SP_MD,
    SP_LG,
    action_button,
    aggregate_row,
    attach_tooltip,
    detail_row,
    empty_state,
    fmt_clock,
    fmt_dur,
    metric_tile,
    rounded_panel,
    sao_entry,
    sao_option_menu,
    sao_scrollbar,
    section_card,
    source_badges,
    source_cn,
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
    _make_panel_close_button,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
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


class ActAggregatePanel:
    """ACT 聚合工作台：插件开发者按维度(技能/怪物/参与者/事件类型/自定义字段)聚合事件，
    可读为主、点开看原始 payload，是给插件调试/抓数据用的检查器。"""

    # 聚合维度选择器：中文标签 ↔ 后端 group_by id
    _DIMENSION_LABELS = ("技能", "怪物/目标", "参与者", "事件类型", "自定义字段")
    _DIMENSION_BY_LABEL = {"技能": "skill", "怪物/目标": "monster", "参与者": "actor",
                           "事件类型": "topic", "自定义字段": "field"}
    _LABEL_BY_DIMENSION = {v: k for k, v in _DIMENSION_BY_LABEL.items()}

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="ACT COCKPIT: --")
        self._status_var = tk.StringVar(value="Ready")
        self._query_var = tk.StringVar(value="")
        self._source_var = tk.StringVar(value="live")
        self._group_by_var = tk.StringVar(value="技能")        # 聚合维度（中文显示）
        self._group_field_var = tk.StringVar(value="")        # 自定义字段名
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[Any, ...] = ()
        self._last_sig = ""
        self._expanded_groups: set[str] = set()
        self._expanded_rows: set[str] = set()                 # 展开看原始 payload 的事件行

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
        query = self._query_var.get()
        source = self._source_var.get()
        group_by = self._DIMENSION_BY_LABEL.get(self._group_by_var.get(), "skill")
        group_field = self._group_field_var.get().strip()
        request_key = (query, source, group_by, group_field)
        if (
            self._last_status
            and request_key == self._last_request_key
            and now - self._last_refresh_at < 0.35
        ):
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_aggregate_status(self.owner, limit=1000, query=query, source=source, window_ms=1000, top_n=20,
                                          group_by=group_by, group_field=group_field)
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "overview": {}, "groups": [], "dimensions": [], "group_by": group_by, "timeline_clusters": [], "skill_damage": [], "monster_damage": [], "dungeon_damage": [], "log_groups": [], "source_mix": [], "raw_counts": {}, "filters": {"query": query, "source": source}, "errors": [str(exc)]}
        try:
            status = dict(status or {})
            status["graph"] = act_graph_timeseries_status(self.owner, limit=120, query=query)
        except Exception as exc:
            status.setdefault("errors", []).append(str(exc))
            status["graph"] = {"ok": False, "series": {}, "row_count": 0, "errors": [str(exc)]}
        # Entity-side plugin render hook (parity with web act_aggregate tap):
        # plugins may transform the cockpit payload before it is rendered.
        try:
            hooked = act_render_apply_hooks(self.owner, 'act_aggregate', status)
            if hooked.get('ok') and isinstance(hooked.get('payload'), dict):
                status = hooked['payload']
        except Exception:
            pass
        self._last_status = status
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(status)
        return status

    def filter(self) -> Dict[str, Any]:
        self._cancel_pending_filter()
        self._last_refresh_at = 0.0
        self._last_request_key = ()
        self._last_sig = ""
        return self.refresh()

    def _cancel_pending_filter(self) -> None:
        pending = getattr(self, '_filter_after_id', None)
        self._filter_after_id = None
        if pending is not None and self._win is not None:
            try:
                self._win.after_cancel(pending)
            except Exception:
                pass

    def _schedule_filter(self) -> None:
        """搜索输入防抖：停止键入 300ms 后自动过滤（与 Web 端行为一致）。"""
        if self._win is None:
            return
        self._cancel_pending_filter()
        try:
            self._filter_after_id = self._win.after(300, self.filter)
        except Exception:
            self._filter_after_id = None

    def copy_json(self) -> Dict[str, Any]:
        text = json.dumps(self._last_status or self.refresh(), ensure_ascii=False, indent=2, default=str)
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Aggregate cockpit JSON copied')
            return {"ok": True, "text": text}
        except Exception as exc:
            self._status_var.set(str(exc))
            return {"ok": False, "message": str(exc), "text": text}

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Cockpit')
        win.geometry('960x862+180+95')
        win.minsize(820, 520)
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
        header = _sao_panel_header(win, 'ACT SEMANTIC COCKPIT', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT SEMANTIC COCKPIT', bg=_SAO_PANEL_BODY_BG,
             fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='AGGREGATE 聚合驾驶舱', bg=_SAO_PANEL_BODY_BG,
             fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))

        control = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        control.pack(side='right', anchor='n', pady=(10, 0))
        self._badge_frame_agg = tk.Frame(control, bg=_SAO_PANEL_BODY_BG)
        self._badge_frame_agg.pack(side='left', padx=(0, 12), anchor='n')
        tk.Label(control, text='聚合维度', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_option_menu(control, self._group_by_var, *self._DIMENSION_LABELS, command=lambda _v: self.filter()).pack(side='left', padx=(6, 6))
        field_entry = sao_entry(control, textvariable=self._group_field_var, width=8)
        field_entry.pack(side='left', padx=(0, 8))
        attach_tooltip(field_entry, '自定义字段维度：聚合维度选「自定义字段」时按此 payload 字段名分组')
        field_entry.bind('<Return>', lambda _e: self.filter())
        tk.Label(control, text='搜索', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        query_entry = sao_entry(control, textvariable=self._query_var, width=14)
        query_entry.pack(side='left', padx=(6, 8))
        # 输入即过滤（300ms 防抖，对齐 Web 端 onChange 自动刷新），回车立即生效
        query_entry.bind('<KeyRelease>', lambda _e: self._schedule_filter())
        query_entry.bind('<Return>', lambda _e: self.filter())
        sao_option_menu(control, self._source_var, 'live', 'history', command=lambda _v: self.filter()).pack(side='left', padx=(0, 8))
        action_button(control, '刷新', self.refresh, kind='gold').pack(side='left', padx=(0, 6))
        _make_panel_close_button(control, self.hide, bg=_SAO_PANEL_BODY_BG, flat=True).pack(side='left', padx=(6, 0))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=14, pady=(0, 14))
        # 右侧概览栏 (OVERVIEW + SOURCE MIX, 对齐 web act-side 300px 侧栏; 圆角)
        side_card, side_inner = rounded_panel(outer, bg=_SAO_PANEL_BODY_BG,
                                              border=_SAO_PANEL_BORDER, radius=10, pad=12)
        side_card.configure(width=300)
        side_card.pack(side='right', fill='y', padx=(12, 0))
        self._side = side_inner
        # 主区 (左): 可滚动语义分组
        main_wrap = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG)
        main_wrap.pack(side='left', fill='both', expand=True)
        canvas = tk.Canvas(main_wrap, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(main_wrap, canvas.yview)
        self._rows = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._rows, anchor='nw')
        # Stretch the scrolled frame to the canvas width so content fills the
        # whole panel instead of hugging the left edge.
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_side(self, status: Mapping[str, Any]) -> None:
        side = getattr(self, '_side', None)
        if side is None:
            return
        for child in list(side.winfo_children()):
            child.destroy()
        overview = status.get('overview') if isinstance(status.get('overview'), Mapping) else {}
        counts = status.get('raw_counts') if isinstance(status.get('raw_counts'), Mapping) else {}
        errors = list(status.get('errors') or [])
        span_s = round(_finite_float(overview.get('span_ms'), 0.0, lo=0.0) / 1000.0, 1)
        pad = tk.Frame(side, bg=_SAO_PANEL_BODY_BG)
        pad.pack(fill='both', expand=True)
        tk.Label(pad, text='OVERVIEW 概览', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_sao_font(9, True), anchor='w').pack(fill='x', pady=(0, 6))
        for key, val in (
            ('当前战斗', overview.get('dungeon_name') or overview.get('mode') or 'live'),
            ('数据源', status.get('source') or 'live'),
            ('战斗时长', f"{self._fmt(span_s)} 秒"),
            ('已持续', f"{self._fmt(overview.get('elapsed_s'))} 秒"),
            ('事件数', _finite_int(counts.get('rows'), 0, lo=0)),
            ('错误', len(errors)),
        ):
            row = tk.Frame(pad, bg=_SAO_PANEL_BODY_BG)
            row.pack(fill='x', pady=2)
            tk.Label(row, text=str(key), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(9), anchor='w').pack(side='left')
            tk.Label(row, text=str(val), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                     font=get_sao_font(9), anchor='e').pack(side='right')
        tk.Label(pad, text='SOURCE MIX 来源', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_sao_font(9, True), anchor='w').pack(fill='x', pady=(14, 6))
        mix = list(status.get('source_mix') or [])
        if mix:
            for src in mix:
                if isinstance(src, Mapping):
                    text = f"{source_cn(src.get('source'))}  {src.get('count') or 0}"
                else:
                    text = source_cn(src)
                tk.Label(pad, text=text, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                         font=get_cjk_font(9), anchor='w').pack(fill='x', pady=1)
        else:
            tk.Label(pad, text='No sources', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(9), anchor='w').pack(fill='x')

    def _render_status(self, status: Mapping[str, Any]) -> None:
        overview = status.get('overview') if isinstance(status.get('overview'), Mapping) else {}
        counts = status.get('raw_counts') if isinstance(status.get('raw_counts'), Mapping) else {}
        errors = list(status.get('errors') or [])
        if hasattr(self, '_badge_frame_agg'):
            for child in list(self._badge_frame_agg.winfo_children()):
                child.destroy()
            badge_text = 'OK' if status.get('ok', True) and not errors else 'ERROR'
            badge_kind = 'ok' if badge_text == 'OK' else 'danger'
            status_badge(self._badge_frame_agg, badge_text, kind=badge_kind).pack(side='left')
        self._summary_var.set(
            f"DMG {self._fmt(overview.get('damage'))} · DPS {self._fmt(overview.get('dps'))} · "
            f"EVENTS {_finite_int(counts.get('rows'), 0, lo=0)} · GROUPS {_finite_int(counts.get('skills'), 0, lo=0)}/{_finite_int(counts.get('monsters'), 0, lo=0)}/{_finite_int(counts.get('dungeons'), 0, lo=0)}"
        )
        self._status_var.set(
            f"{overview.get('dungeon_name') or overview.get('mode') or 'live'} · source={status.get('source') or 'live'} · "
            f"query={self._query_var.get() or '-'} · errors={len(errors)}"
        )
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        self._render_side(status)
        # 重建前记滚动位置, 重建后还原 — web 端 setContentHtml(preserveScroll)
        # 同款; 否则每次刷新滚动跳回顶部
        canvas = getattr(self, '_canvas', None)
        try:
            scroll_y = canvas.yview()[0] if canvas is not None else 0.0
        except Exception:
            scroll_y = 0.0
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_header_summary(status)
        if _finite_int(counts.get('rows'), 0, lo=0) <= 0:
            empty_state(self._rows, '等待 ACT 事件 / 战斗数据', '聚合驾驶舱会在收到伤害、技能、怪物、地牢或日志事件后自动显示语义分组。').pack(fill='x', padx=4, pady=10)
            return
        # 工作台：只显示当前选中的聚合维度（插件开发者可在工具栏切换"按什么聚合"）。
        dim_label = self._LABEL_BY_DIMENSION.get(str(status.get('group_by') or 'skill'), '技能')
        if str(status.get('group_by') or '') == 'field':
            dim_label = f"字段 {status.get('group_field') or '?'}"
        groups = status.get('groups') or []
        accent = {'skill': 'gold', 'monster': 'danger', 'actor': 'cyan', 'topic': 'gold', 'field': 'cyan'}.get(str(status.get('group_by') or 'skill'), 'gold')
        self._render_group_section(f'按 {dim_label} 聚合', '', 'g', groups, value_key='damage', accent=accent)
        self._render_graph_preview(status.get('graph') if isinstance(status.get('graph'), Mapping) else {})
        if canvas is not None and scroll_y > 0:
            try:
                self._rows.update_idletasks()
                canvas.configure(scrollregion=canvas.bbox('all'))
                canvas.yview_moveto(scroll_y)
            except Exception:
                pass

    def _render_header_summary(self, status: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        overview = status.get('overview') if isinstance(status.get('overview'), Mapping) else {}
        counts = status.get('raw_counts') if isinstance(status.get('raw_counts'), Mapping) else {}
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 10))
        items = (
            ('总伤害', self._fmt(overview.get('damage')), f"{_finite_int(counts.get('rows'), 0, lo=0)} 次事件", 'gold'),
            ('每秒伤害 DPS', self._fmt(overview.get('dps')), f"持续 {self._fmt(overview.get('elapsed_s'))} 秒", 'cyan'),
            ('治疗 / 每秒治疗', f"{self._fmt(overview.get('heal'))} / {self._fmt(overview.get('hps'))}", '辅助治疗', 'heal'),
            ('技能种类', _finite_int(counts.get('skills'), 0, lo=0), '种技能', 'gold'),
            ('怪物数', _finite_int(counts.get('monsters'), 0, lo=0), '个目标', 'danger'),
            ('地牢 / 场景', _finite_int(counts.get('dungeons'), 0, lo=0), overview.get('dungeon_name') or '当前', 'cyan'),
        )
        # 3 列网格 (与窄主区 + web auto-fit 一致), 避免 6 卡挤一行导致数值出框
        cols = 3
        for col in range(cols):
            grid.columnconfigure(col, weight=1, uniform='kpi')
        for idx, (label, value, sub, accent) in enumerate(items):
            metric_tile(grid, label, value, sub=str(sub), accent=accent).grid(
                row=idx // cols, column=idx % cols, sticky='nsew', padx=3, pady=3)
        badges = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        badges.pack(fill='x', padx=4, pady=(0, 8))
        status_badge(badges, f"模式 {overview.get('mode') or 'live'}", kind='cyan').pack(side='left', padx=(0, SP_SM))
        span_s = round(_finite_float(overview.get('span_ms'), 0.0, lo=0.0) / 1000.0, 1)
        status_badge(badges, f"战斗时长 {self._fmt(span_s)} 秒", kind='gold').pack(side='left', padx=(0, SP_SM))
        source_badges(badges, status.get('source_mix') or []).pack(side='left')

    def _render_group_section(self, title: str, subtitle: str, prefix: str, groups: list[Any], *, value_key: str, accent: str) -> None:
        if self._rows is None:
            return
        box = section_card(self._rows, title, subtitle=subtitle, badge=str(len(groups)), accent=accent)
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=SP_SM, pady=SP_SM)
        valid = [item for item in groups if isinstance(item, Mapping)]
        if not valid:
            empty_state(body, '暂无聚合数据', '当前筛选条件下没有可展示的语义分组。').pack(fill='x')
            return
        max_value = max(1.0, *[_finite_float(item.get(value_key) or item.get('damage') or item.get('total_value'), 0.0, lo=0.0) for item in valid])
        for idx, group in enumerate(valid[:20]):
            key = f"{prefix}:{group.get('key') or idx}"
            value = _finite_float(group.get(value_key) or group.get('damage') or group.get('total_value'), 0.0, lo=0.0)
            meta = self._group_meta(group)
            row = aggregate_row(
                body,
                title=str(group.get('name') or group.get('key') or '-'),
                meta=meta,
                value=f"{self._fmt(value)} · {_finite_int(group.get('count'), 0, lo=0)}x",
                ratio=value / max_value if max_value else 0.0,
                accent=accent,
                zebra=bool(idx % 2),
                command=lambda k=key: self._toggle_group(k),
                expanded=key in self._expanded_groups,
            )
            row.pack(fill='x', pady=2)
            if key in self._expanded_groups:
                self._render_group_details(body, group, accent=accent)

    def _render_group_details(self, parent: tk.Misc, group: Mapping[str, Any], *, accent: str = 'cyan') -> None:
        detail = tk.Frame(parent, bg=_SAO_PANEL_BODY_BG)
        detail.pack(fill='x', padx=(SP_LG, SP_XS), pady=(0, SP_SM))
        chips = tk.Frame(detail, bg=_SAO_PANEL_BODY_BG)
        chips.pack(fill='x', pady=(2, SP_XS))
        for label, values, kind in (
            ('参与者', group.get('actors') or group.get('actor_uids') or [], 'cyan'),
            ('目标', group.get('targets') or group.get('target_uids') or [], 'gold'),
            ('技能', group.get('skills') or group.get('skill_ids') or [], 'gold'),
            ('来源', [source_cn(s) for s in (group.get('sources') or [])], 'cyan'),
        ):
            values = list(values or [])[:4]
            if values:
                status_badge(chips, f"{label}: {', '.join(str(v) for v in values)}", kind=kind).pack(side='left', padx=(0, SP_SM), pady=2)
        rows = [row for row in list(group.get('rows') or []) if isinstance(row, Mapping)][:6]
        if not rows:
            return
        tk.Label(detail, text='代表事件（点击行展开原始 payload / 含 UID、epoch）', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8), anchor='w').pack(fill='x', pady=(SP_XS, 1))
        for ridx, row in enumerate(rows):
            row_key = f"{group.get('key')}:{ridx}"
            open_raw = row_key in self._expanded_rows
            detail_row(detail, self._readable_event_line(row), accent=accent, zebra=bool(ridx % 2),
                       command=lambda k=row_key: self._toggle_row(k), expanded=open_raw).pack(fill='x', pady=1)
            if open_raw:
                self._render_raw_payload(detail, row)

    def _render_raw_payload(self, parent: tk.Misc, row: Mapping[str, Any]) -> None:
        """The full event as the plugin sees it — raw payload + raw UIDs + epoch time."""
        raw = dict(row.get('payload') if isinstance(row.get('payload'), Mapping) else {})
        # 行级解析名键是 'skill'/'monster' (_action_log_group_metadata),
        # 'skill_name'/'monster_name' 只可能来自 payload 本身
        for key in ('time_ms', 'topic', 'kind', 'actor', 'actor_uid', 'target', 'target_uid',
                    'skill_id', 'skill', 'skill_name', 'monster_id', 'monster', 'monster_name',
                    'value', 'damage', 'heal', 'source'):
            if row.get(key) not in (None, '') and key not in raw:
                raw[key] = row.get(key)
        text = json.dumps(raw, ensure_ascii=False, indent=2, default=str)
        box = tk.Frame(parent, bg='#0f1720', highlightthickness=1, highlightbackground=_SAO_PANEL_GOLD)
        box.pack(fill='x', padx=(SP_LG, 0), pady=(1, SP_XS))
        tk.Label(box, text=text, bg='#0f1720', fg='#d7f7ff', font=('Consolas', 8), anchor='w',
                 justify='left', wraplength=900).pack(fill='x', padx=SP_SM, pady=SP_XS)

    def _toggle_row(self, row_key: str) -> None:
        if row_key in self._expanded_rows:
            self._expanded_rows.remove(row_key)
        else:
            self._expanded_rows.add(row_key)
        self._last_sig = ""
        self._render_status(self._last_status)

    def _render_graph_preview(self, graph: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        box = section_card(self._rows, '趋势预览', subtitle='', badge=str(graph.get('row_count') or 0))
        box.pack(fill='x', padx=4, pady=(9, 0))
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=8, pady=8)
        series = graph.get('series') if isinstance(graph.get('series'), Mapping) else {}
        for metric, title, accent in (('damage', '总伤害', 'gold'), ('heal', '治疗', 'heal'), ('event_count', '事件数', 'cyan')):
            item = series.get(metric) if isinstance(series.get(metric), Mapping) else {}
            points = list(item.get('points') or [])
            values = [_finite_float(p.get('value'), 0.0, lo=0.0) for p in points if isinstance(p, Mapping)]
            latest = values[-1] if values else 0.0
            peak = max(values) if values else 0.0
            ratio = (latest / peak) if peak > 0 else 0.0
            aggregate_row(body, title=title, meta=f"{len(points)} 个采样点 · 峰值 {self._fmt(peak)}",
                          value=self._fmt(latest), ratio=ratio, accent=accent).pack(fill='x', pady=2)

    def _readable_event_line(self, row: Mapping[str, Any]) -> str:
        """One representative event as a human line: 时钟 · 类型 · 来源→目标 · 标签 · 值
        (drops the meaningless 'tcp → x · 0' noise: source-as-actor humanized, 0 value hidden)."""
        clock = fmt_clock(row.get('time_ms'))
        topic = topic_cn(row.get('topic'))
        actor = str(row.get('actor') or '').strip()
        target = str(row.get('target') or '').strip()
        label = str(row.get('label') or '').strip()
        value = self._fmt(row.get('value'))
        parts = [clock, topic]
        actor_disp = source_cn(actor) if actor else ''
        if actor_disp and target:
            parts.append(f"{actor_disp} → {target}")
        elif target or actor_disp:
            parts.append(target or actor_disp)
        raw_topic = str(row.get('topic') or '').strip().lower()
        if label and label.lower() != raw_topic and label not in (target, topic):
            parts.append(label)
        if value and value not in ('0', '0.00'):
            parts.append(value)
        return ' · '.join(part for part in parts if part)

    def _group_meta(self, group: Mapping[str, Any]) -> str:
        first_ms = _finite_int(group.get('first_time_ms'), 0, lo=0)
        last_ms = _finite_int(group.get('last_time_ms'), first_ms, lo=0)
        span = _finite_int(group.get('duration_ms'), max(0, last_ms - first_ms), lo=0)
        bits = [f"持续 {fmt_dur(span)}"]
        for key, label in (('targets', '个目标'), ('monsters', '个怪物'), ('skills', '种技能'), ('actors', '名参与者'), ('dungeons', '个场景')):
            values = list(group.get(key) or [])
            if values:
                bits.append(f"{len(values)} {label}")
        srcs: list[str] = []
        for src in (group.get('sources') or []):
            name = source_cn(src)
            if name and name not in srcs:
                srcs.append(name)
        if srcs:
            bits.append('来源 ' + '/'.join(srcs))
        return ' · '.join(bits)

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
        if not math.isfinite(number):
            number = 0.0
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        return str(int(number)) if number == int(number) else f"{number:.2f}"

    def _signature(self, status: Mapping[str, Any]) -> str:
        counts = status.get('raw_counts') if isinstance(status.get('raw_counts'), Mapping) else {}
        overview = status.get('overview') if isinstance(status.get('overview'), Mapping) else {}
        groups_sig = []
        for item in list(status.get('groups') or [])[:20]:
            if not isinstance(item, Mapping):
                continue
            row_sig = []
            for ridx, row in enumerate(list(item.get('rows') or [])[:6]):
                if not isinstance(row, Mapping):
                    continue
                row_key = f"{item.get('key')}:{ridx}"
                row_sig.append((
                    row.get('time_ms'), row.get('topic'), row.get('source'),
                    row.get('actor'), row.get('actor_uid'),
                    row.get('target'), row.get('target_uid'),
                    row.get('label'), row.get('value'),
                    row.get('payload') if row_key in self._expanded_rows else None,
                ))
            groups_sig.append((
                item.get('key'), item.get('name'),
                item.get('damage'), item.get('heal'), item.get('total_value'), item.get('count'),
                item.get('duration_ms'), item.get('first_time_ms'), item.get('last_time_ms'),
                tuple(item.get('actors') or item.get('actor_uids') or ()),
                tuple(item.get('targets') or item.get('target_uids') or ()),
                tuple(item.get('skills') or item.get('skill_ids') or ()),
                tuple(item.get('sources') or ()),
                tuple(item.get('dungeons') or ()),
                row_sig,
            ))
        graph = status.get('graph') if isinstance(status.get('graph'), Mapping) else {}
        series = graph.get('series') if isinstance(graph.get('series'), Mapping) else {}
        graph_sig = []
        for metric in ('damage', 'heal', 'event_count'):
            item = series.get(metric) if isinstance(series.get(metric), Mapping) else {}
            graph_sig.append((
                metric,
                tuple((p.get('time_ms'), p.get('topic'), p.get('value'), p.get('row_id'))
                      for p in list(item.get('points') or [])[-24:]
                      if isinstance(p, Mapping)),
            ))
        return repr((
            tuple(sorted((counts or {}).items())),
            overview.get('damage'), overview.get('heal'), overview.get('dps'), overview.get('hps'),
            overview.get('elapsed_s'), overview.get('span_ms'), overview.get('dungeon_name'), overview.get('mode'),
            tuple((src.get('source'), src.get('count')) for src in list(status.get('source_mix') or []) if isinstance(src, Mapping)),
            str(status.get('group_by') or ''), str(status.get('group_field') or ''),
            groups_sig,
            graph_sig,
            tuple(sorted(self._expanded_groups)),
            tuple(sorted(self._expanded_rows)),
            self._query_var.get(), self._source_var.get(),
        ))
