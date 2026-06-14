# -*- coding: utf-8 -*-
"""Entity-mode ACT combatant drilldown panel."""

from __future__ import annotations

import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_combatant_drilldown_back,
    act_combatant_drilldown_filter,
    act_combatant_drilldown_focus_target,
    act_combatant_drilldown_status,
)
from gui_modules.sao_panel_components import (
    SP_SM,
    SP_MD,
    action_button,
    aggregate_row,
    empty_state,
    keep_canvas_scroll,
    metric_tile,
    more_indicator,
    rounded_panel,
    sao_entry,
    sao_scrollbar,
    section_card,
    status_badge,
    topic_cn,
)
from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules.sao_panel_ui import (
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


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    num = int(_finite_float(value, float(default), lo=lo, hi=hi))
    if lo is not None:
        num = max(int(lo), num)
    if hi is not None:
        num = min(int(hi), num)
    return num


def _mapping_items(value: Any) -> list[Mapping[str, Any]]:
    if not isinstance(value, (list, tuple)):
        return []
    return [item for item in value if isinstance(item, Mapping)]


def _list_count(value: Any) -> int:
    return len(value) if isinstance(value, (list, tuple)) else 0


class CombatantDrilldownPanel:
    """SAO-styled combatant detail panel for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="COMBATANT: --")
        self._status_var = tk.StringVar(value="Ready")
        self._combatant_var = tk.StringVar(value="")
        self._query_var = tk.StringVar(value="")
        self._focus_var = tk.StringVar(value="")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[Any, ...] = ()
        self._last_sig = ""

    def select(self, combatant_id: Any = "", *, query: str = "") -> Dict[str, Any]:
        if combatant_id is not None:
            self._combatant_var.set(str(combatant_id or ""))
        if query:
            self._query_var.set(str(query or ""))
        self._last_refresh_at = 0.0
        self._last_request_key = ()
        self._last_sig = ""
        self.show()
        return self.refresh()

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
        combatant_id = self._combatant_var.get()
        query = self._query_var.get()
        focus_target = self._focus_var.get()
        request_key = (combatant_id, query, focus_target)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_combatant_drilldown_status(
                self.owner,
                combatant_id=combatant_id,
                query=query,
                focus_target=focus_target,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "summary": {}, "skills": [], "incoming": [], "outgoing": [], "filters": {"query": query, "focus_target": focus_target}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def filter(self) -> Dict[str, Any]:
        return self._apply_result(act_combatant_drilldown_filter(self.owner, combatant_id=self._combatant_var.get(), query=self._query_var.get()), 'FILTER APPLIED')

    def focus_target(self) -> Dict[str, Any]:
        return self._apply_result(act_combatant_drilldown_focus_target(self.owner, combatant_id=self._combatant_var.get(), target_id=self._focus_var.get()), 'TARGET FOCUSED')

    def back(self) -> Dict[str, Any]:
        self._combatant_var.set('')
        return self._apply_result(act_combatant_drilldown_back(self.owner), 'BACK')

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
        win.title('SAO ACT Combatant Drilldown')
        win.geometry('960x862+280+155')
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
        header = _sao_panel_header(win, 'ACT COMBATANT DRILLDOWN', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        # ── title area (matches aggregate cockpit pattern) ──
        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT COMBATANT', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='COMBATANT 成员钻取', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))

        # ── control bar (right-aligned) ──
        control = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        control.pack(side='right', anchor='n', pady=(10, 0))
        self._badge_frame_cd = tk.Frame(control, bg=_SAO_PANEL_BODY_BG)
        self._badge_frame_cd.pack(side='left', padx=(0, 12), anchor='n')
        tk.Label(control, text='UID', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(control, textvariable=self._combatant_var, width=14).pack(side='left', padx=(6, 8))
        tk.Label(control, text='搜索', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        query_entry = sao_entry(control, textvariable=self._query_var, width=14)
        query_entry.pack(side='left', padx=(6, 8))
        query_entry.bind('<Return>', lambda _e: self.filter())
        tk.Label(control, text='目标', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(control, textvariable=self._focus_var, width=12).pack(side='left', padx=(6, 8))
        action_button(control, '聚焦', self.focus_target, kind='gold').pack(side='left', padx=(0, 6))
        action_button(control, '刷新', self.refresh, kind='gold').pack(side='left', padx=(0, 6))
        action_button(control, '返回', self.back).pack(side='left', padx=(0, 6))
        _make_panel_close_button(control, self.hide, bg=_SAO_PANEL_BODY_BG, flat=True).pack(side='left', padx=(6, 0))

        # ── combatant identity label ──
        self._identity_label = tk.Label(body, textvariable=self._summary_var, anchor='w',
                                        bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                                        font=get_cjk_font(10, True))
        self._identity_label.pack(fill='x', padx=14, pady=(0, 6))

        # ── two-column layout: left main + right sidebar 280px ──
        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=14, pady=(0, 14))

        # right sidebar (combatants list)
        side_card, side_inner = rounded_panel(outer, bg=_SAO_PANEL_BODY_BG,
                                              border=_SAO_PANEL_BORDER, radius=10, pad=12)
        side_card.configure(width=280)
        side_card.pack(side='right', fill='y', padx=(12, 0))
        self._side = side_inner

        # left main area (scrollable)
        main_wrap = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG)
        main_wrap.pack(side='left', fill='both', expand=True)
        canvas = tk.Canvas(main_wrap, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(main_wrap, canvas.yview)
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
        if hasattr(self, '_badge_frame_cd'):
            for child in list(self._badge_frame_cd.winfo_children()):
                child.destroy()
            badge_text = 'READY' if status.get('ok', True) and not list(status.get('errors') or []) else 'ERROR'
            badge_kind = 'ok' if badge_text == 'READY' else 'danger'
            status_badge(self._badge_frame_cd, badge_text, kind=badge_kind).pack(side='left')
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        skills = _mapping_items(status.get('skills'))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cid = str(status.get('combatant_id') or self._combatant_var.get() or '')
        if cid and self._combatant_var.get() != cid:
            self._combatant_var.set(cid)
        name = summary.get('name') or cid or '--'
        profession = str(summary.get('profession') or '')
        uid_display = cid or '--'
        identity_parts = [name]
        if profession:
            identity_parts.append(profession)
        identity_parts.append(f"UID {uid_display}")
        self._summary_var.set(" · ".join(identity_parts))
        self._status_var.set(f"encounter={status.get('encounter_id') or 'live'} · query={filters.get('query') or '-'} · focus={filters.get('focus_target') or '-'} · errors={_list_count(status.get('errors'))}")
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_sidebar(status)
        if not summary:
            self._render_empty()
            return
        self._render_metrics(summary)
        self._render_skills(skills)

    def _render_empty(self) -> None:
        if self._rows is None:
            return
        empty_state(self._rows, '请输入 combatant UID',
                    '可从 DPS 面板或 ACT 数据中选择角色。').pack(fill='x', padx=4, pady=10)

    def _render_metrics(self, summary: Mapping[str, Any]) -> None:
        """Render the 4 KPI metric tiles row."""
        if self._rows is None:
            return
        hits = _finite_int(summary.get('hits'), 0, lo=0)
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 10))
        items = (
            ('总伤害', self._fmt(summary.get('damage')), '', 'gold'),
            ('DPS', self._fmt(summary.get('dps')) + '/s', '', 'cyan'),
            ('暴击率', self._pct_int(summary.get('crit_rate')), '', 'gold'),
            ('命中', f"{hits:,}" if hits else '0', '', 'cyan'),
        )
        cols = len(items)
        for col in range(cols):
            grid.columnconfigure(col, weight=1, uniform='kpi')
        for idx, (label, value, sub, accent) in enumerate(items):
            metric_tile(grid, label, value, sub=sub, accent=accent).grid(
                row=0, column=idx, sticky='nsew', padx=3, pady=3)

    def _render_skills(self, skills: list[Mapping[str, Any]]) -> None:
        """Render collapsible skill breakdown section using aggregate_row."""
        if self._rows is None:
            return
        box = section_card(self._rows,
                           '▼ 技能贡献 Skill breakdown',
                           subtitle='',
                           badge=str(len(skills)),
                           accent='gold')
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=SP_SM, pady=SP_SM)
        if not skills:
            empty_state(body, '暂无技能数据').pack(fill='x')
            return
        max_amount = max(1, *[_finite_int(sk.get('amount'), 0, lo=0) for sk in skills])
        for idx, skill in enumerate(skills[:40]):
            amount = _finite_int(skill.get('amount'), 0, lo=0)
            hits = _finite_int(skill.get('hits'), 0, lo=0)
            crit = self._pct_int(skill.get('crit_rate'))
            is_heal = skill.get('kind') == 'heal'
            accent = 'heal' if is_heal else 'gold'
            row = aggregate_row(
                body,
                title=str(skill.get('name') or '-'),
                meta=f"暴击率{crit}·命中 {hits}",
                value=f"{self._fmt(amount)} · {hits}x",
                ratio=amount / max_amount if max_amount else 0.0,
                accent=accent,
                zebra=bool(idx % 2),
                command=lambda sk=skill: self._open_skill(sk),
            )
            row.pack(fill='x', pady=2)
        hidden = len(skills) - 40
        if hidden > 0:
            more_indicator(body, hidden, noun='个技能').pack(fill='x', padx=4, pady=(2, 0))

    def _open_skill(self, skill: Mapping[str, Any]) -> None:
        sid = str(skill.get('skill_id') or skill.get('base_skill_id') or skill.get('source_skill_id') or '')
        cid = str(self._combatant_var.get() or self._last_status.get('combatant_id') or '')
        if not sid or not cid:
            self._status_var.set('SKILL DRILLDOWN NEEDS UID + SKILL ID')
            return
        panel = getattr(self.owner, '_act_skill_drilldown_panel', None)
        if panel is None:
            try:
                from gui_modules.sao_gui_skill_drilldown import SkillDrilldownPanel
                panel = SkillDrilldownPanel(self.root, self.owner)
                setattr(self.owner, '_act_skill_drilldown_panel', panel)
            except Exception as exc:
                self._status_var.set(f'SKILL PANEL ERROR: {exc}')
                return
        select = getattr(panel, 'select', None)
        if callable(select):
            select(cid, sid)
        else:
            try:
                panel.show()
            except Exception:
                pass
        self._status_var.set(f'OPEN SKILL {sid}')

    def _render_interactions(self, status: Mapping[str, Any]) -> None:
        """Render outgoing / incoming / focus section using section_card + aggregate_row."""
        if self._rows is None:
            return
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        outgoing = _mapping_items(status.get('outgoing'))[:8]
        incoming = _mapping_items(status.get('incoming'))[:5]
        focus = filters.get('focus_target')
        if not outgoing and not incoming and not focus:
            return
        # outgoing section
        if outgoing or focus:
            out_items = []
            if focus:
                out_items.append({'kind': 'focus', 'name': str(focus), 'amount': 0})
            out_items.extend(outgoing)
            box = section_card(self._rows, '输出目标 Outgoing', badge=str(len(out_items)), accent='cyan')
            box.pack(fill='x', padx=4, pady=(SP_MD, 0))
            body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
            body.pack(fill='x', padx=SP_SM, pady=SP_SM)
            max_out = max(1, *[_finite_int(i.get('amount'), 0, lo=0) for i in out_items])
            for idx, item in enumerate(out_items):
                amt = _finite_int(item.get('amount'), 0, lo=0)
                aggregate_row(
                    body,
                    title=str(item.get('name') or '-'),
                    meta=topic_cn(item.get('kind'), default='输出'),
                    value=self._fmt(amt) if amt else '',
                    ratio=amt / max_out if max_out else 0.0,
                    accent='cyan',
                    zebra=bool(idx % 2),
                ).pack(fill='x', pady=1)
        # incoming section
        if incoming:
            box = section_card(self._rows, '承受来源 Incoming', badge=str(len(incoming)), accent='danger')
            box.pack(fill='x', padx=4, pady=(SP_MD, 0))
            body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
            body.pack(fill='x', padx=SP_SM, pady=SP_SM)
            max_in = max(1, *[_finite_int(i.get('amount', i.get('value')), 0, lo=0) for i in incoming])
            for idx, item in enumerate(incoming):
                raw_amt = item.get('amount') if item.get('amount') is not None else item.get('value')
                amt = _finite_int(raw_amt, 0, lo=0)
                aggregate_row(
                    body,
                    title=str(item.get('name') or item.get('topic') or '-'),
                    meta=topic_cn(item.get('kind'), default='承受'),
                    value=self._fmt(amt) if amt else '',
                    ratio=amt / max_in if max_in else 0.0,
                    accent='danger',
                    zebra=bool(idx % 2),
                ).pack(fill='x', pady=1)

    def _render_sidebar(self, status: Mapping[str, Any]) -> None:
        """Render the right sidebar with party member cards (name + class + damage)."""
        side = getattr(self, '_side', None)
        if side is None:
            return
        for child in list(side.winfo_children()):
            child.destroy()
        pad = tk.Frame(side, bg=_SAO_PANEL_BODY_BG)
        pad.pack(fill='both', expand=True)
        tk.Label(pad, text='COMBATANTS 成员', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_sao_font(9, True), anchor='w').pack(fill='x', pady=(0, 6))
        # Gather party members from the DPS report (sorted by damage desc)
        combatants = self._gather_party_members(status)
        current_uid = str(status.get('combatant_id') or self._combatant_var.get() or '')
        if combatants:
            for member in combatants[:12]:
                m_uid = str(member.get('uid') or member.get('id') or '')
                m_name = str(member.get('name') or member.get('display_name') or '--')
                m_prof = str(member.get('profession') or member.get('profession_name') or '')
                m_dmg = self._fmt(member.get('damage') or member.get('damage_total') or 0)
                highlight = bool(current_uid and m_uid == current_uid)
                self._sidebar_card(pad, m_name, m_dmg, profession=m_prof, highlight=highlight)
        else:
            # Fallback: show just the current combatant if no party data
            summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
            if summary:
                self._sidebar_card(pad, summary.get('name') or '--',
                                   self._fmt(summary.get('damage')),
                                   profession=str(summary.get('profession') or ''),
                                   highlight=True)
            else:
                tk.Label(pad, text='无成员数据', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                         font=get_cjk_font(9), anchor='w').pack(fill='x', pady=6)

    def _gather_party_members(self, status: Mapping[str, Any]) -> list[dict[str, Any]]:
        """Collect party member list from the DPS tracker / report for sidebar display."""
        members: list[dict[str, Any]] = []
        try:
            tracker = getattr(self.owner, '_dps_tracker', None)
            get_last = getattr(tracker, 'get_last_report', None) if tracker else None
            report = get_last() if callable(get_last) else None
            if isinstance(report, Mapping):
                for row in list(report.get('entities') or []):
                    if isinstance(row, Mapping):
                        members.append(dict(row))
        except Exception:
            pass
        if not members:
            # Fallback: try the DPS history store
            try:
                store = getattr(self.owner, '_dps_history_store', None)
                latest = getattr(store, 'latest_report', None) if store else None
                report = latest() if callable(latest) else None
                if isinstance(report, Mapping):
                    for row in list(report.get('entities') or []):
                        if isinstance(row, Mapping):
                            members.append(dict(row))
            except Exception:
                pass
        # Sort by damage descending
        members.sort(key=lambda m: int(m.get('damage') or m.get('damage_total') or 0), reverse=True)
        return members

    def _sidebar_card(self, parent: tk.Misc, name: str, value: str, *,
                       profession: str = '', highlight: bool = False) -> None:
        """One player card in the sidebar: name + class subtitle + right-aligned damage."""
        bg = _SAO_PANEL_HEADER_BG if highlight else _SAO_PANEL_BODY_BG
        card = tk.Frame(parent, bg=bg, highlightthickness=1,
                        highlightbackground=_SAO_PANEL_GOLD if highlight else _SAO_PANEL_BORDER)
        card.pack(fill='x', pady=2)
        # Top row: name (left) + damage value (right)
        top = tk.Frame(card, bg=bg)
        top.pack(fill='x', padx=8, pady=(5, 0))
        tk.Label(top, text=str(name), bg=bg, fg=_SAO_PANEL_VALUE_FG,
                 font=get_cjk_font(9, True), anchor='w').pack(side='left')
        tk.Label(top, text=str(value), bg=bg,
                 fg=_SAO_PANEL_GOLD if highlight else _SAO_PANEL_LABEL_FG,
                 font=get_sao_font(9), anchor='e').pack(side='right')
        # Subtitle: profession / class
        sub_text = str(profession) if profession else ''
        tk.Label(card, text=sub_text, bg=bg, fg=_SAO_PANEL_LABEL_FG,
                 font=get_cjk_font(8), anchor='w').pack(fill='x', padx=8, pady=(1, 5))

    @staticmethod
    def _fmt(value: Any) -> str:
        number = _finite_float(value, 0.0)
        if abs(number) >= 1_000_000:
            return f"{number / 1_000_000:.2f}m"
        if abs(number) >= 1_000:
            return f"{number / 1_000:.1f}k"
        return str(int(number)) if number == int(number) else f"{number:.2f}"

    @staticmethod
    def _pct(value: Any) -> str:
        return f"{_finite_float(value, 0.0, lo=0.0, hi=1.0) * 100:.1f}%"

    @staticmethod
    def _pct_int(value: Any) -> str:
        """Integer-format percentage (e.g. 38%) matching the webref tile style."""
        return f"{int(round(_finite_float(value, 0.0, lo=0.0, hi=1.0) * 100))}%"

    @staticmethod
    def _signature(status: Mapping[str, Any]) -> str:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        skills = []
        for skill in _mapping_items(status.get('skills'))[:40]:
            skills.append((
                skill.get('skill_id'),
                skill.get('base_skill_id'),
                skill.get('source_skill_id'),
                skill.get('name'),
                skill.get('amount'),
                skill.get('hits'),
                skill.get('kind'),
                skill.get('crit_rate'),
            ))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        outgoing = []
        for item in _mapping_items(status.get('outgoing'))[:8]:
            outgoing.append((item.get('kind'), item.get('name'), item.get('amount')))
        incoming = []
        for item in _mapping_items(status.get('incoming'))[:5]:
            incoming.append((item.get('kind'), item.get('name'), item.get('topic'), item.get('amount'), item.get('value')))
        return repr((
            status.get('combatant_id'),
            summary.get('name'),
            summary.get('damage'),
            summary.get('heal'),
            summary.get('dps'),
            summary.get('hps'),
            summary.get('crit_rate'),
            summary.get('damage_pct'),
            filters.get('query'),
            filters.get('focus_target'),
            skills,
            outgoing,
            incoming,
        ))
