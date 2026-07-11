# -*- coding: utf-8 -*-
# Entity-mode ACT skill drilldown panel.

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_skill_drilldown_back,
    act_skill_drilldown_copy,
    act_skill_drilldown_filter,
    act_skill_drilldown_status,
)
from gui_modules.sao_panel_components import (
    SP_SM,
    _pc,
    action_button,
    empty_state,
    fmt_clock,
    keep_canvas_scroll,
    metric_tile,
    more_indicator,
    rounded_panel,
    sao_entry,
    sao_scrollbar,
    section_card,
    status_badge,
)
from plugins.star_resonance_plugin.panels.panel_text import topic_cn
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
    global _SAO_PANEL_BG, _SAO_PANEL_BODY_BG, _SAO_PANEL_BORDER
    global _SAO_PANEL_GOLD, _SAO_PANEL_HEADER_BG, _SAO_PANEL_LABEL_FG, _SAO_PANEL_VALUE_FG
    _SAO_PANEL_BG = _pc('bg', ui._SAO_PANEL_BG)
    _SAO_PANEL_BODY_BG = _pc('body_bg', ui._SAO_PANEL_BODY_BG)
    _SAO_PANEL_BORDER = _pc('border', ui._SAO_PANEL_BORDER)
    _SAO_PANEL_GOLD = _pc('gold', ui._SAO_PANEL_GOLD)
    _SAO_PANEL_HEADER_BG = _pc('header_bg', ui._SAO_PANEL_HEADER_BG)
    _SAO_PANEL_LABEL_FG = _pc('label_fg', ui._SAO_PANEL_LABEL_FG)
    _SAO_PANEL_VALUE_FG = _pc('value_fg', ui._SAO_PANEL_VALUE_FG)


_refresh_panel_palette()


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


class SkillDrilldownPanel:
    # SAO-styled per-skill detail panel for Entity/Tk.

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="SKILL: --")
        self._status_var = tk.StringVar(value="Ready")
        self._combatant_var = tk.StringVar(value="")
        self._skill_var = tk.StringVar(value="")
        self._query_var = tk.StringVar(value="")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[Any, ...] = ()
        self._last_sig = ""
        self._expanded_refs: set[str] = set()

    def select(self, combatant_id: Any = "", skill_id: Any = "", *, query: str = "") -> Dict[str, Any]:
        if combatant_id is not None:
            self._combatant_var.set(str(combatant_id or ""))
        if skill_id is not None:
            self._skill_var.set(str(skill_id or ""))
        if query:
            self._query_var.set(str(query or ""))
        self._last_refresh_at = 0.0
        self._last_request_key = ()
        self._last_sig = ""
        self._expanded_refs.clear()
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
        skill_id = self._skill_var.get()
        query = self._query_var.get()
        request_key = (combatant_id, skill_id, query)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_skill_drilldown_status(
                self.owner,
                combatant_id=combatant_id,
                skill_id=skill_id,
                query=query,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "summary": {}, "timeline_refs": [], "casts": 0, "hits": 0, "crit_rate": 0.0, "filters": {"query": query}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def filter(self) -> Dict[str, Any]:
        return self._apply_result(act_skill_drilldown_filter(self.owner, combatant_id=self._combatant_var.get(), skill_id=self._skill_var.get(), query=self._query_var.get()), 'FILTER APPLIED')

    def copy(self) -> Dict[str, Any]:
        result = self._apply_result(act_skill_drilldown_copy(self.owner, combatant_id=self._combatant_var.get(), skill_id=self._skill_var.get(), query=self._query_var.get()), 'COPY READY')
        text = str(result.get('text') or '')
        if text:
            try:
                self.root.clipboard_clear()
                self.root.clipboard_append(text)
            except Exception:
                self._status_var.set('Copy payload ready, but clipboard copy failed')
        return result

    def back(self) -> Dict[str, Any]:
        self._combatant_var.set('')
        self._skill_var.set('')
        self._expanded_refs.clear()
        return self._apply_result(act_skill_drilldown_back(self.owner), 'BACK')

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
        _refresh_panel_palette()
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Skill Drilldown')
        win.geometry('960x862+310+180')
        win.minsize(660, 400)
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
        header = _sao_panel_header(win, 'ACT SKILL DRILLDOWN', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        # ── Row 1: toolbar (webref layout) ─────────────────────────────────
        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(7, 2))
        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT SKILL', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='SKILL DRILLDOWN 技能钻取', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))

        # ALL right-side controls in ONE frame, bottom-aligned with title
        controls = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        controls.pack(side='right', anchor='center')
        self._badge_frame_sd = tk.Frame(controls, bg=_SAO_PANEL_BODY_BG)
        self._badge_frame_sd.pack(side='left', padx=(0, 8))
        sao_entry(controls, textvariable=self._query_var, width=18).pack(side='left', padx=(0, 8))
        action_button(controls, '刷新', self.refresh, kind='gold').pack(side='left', padx=(0, 6))
        _make_panel_close_button(controls, self.hide, bg=_SAO_PANEL_BODY_BG, flat=True).pack(side='left', padx=(6, 0))

        # ── Row 2: secondary controls ──────────────────────────────────────
        ctrl2 = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        ctrl2.pack(fill='x', padx=14, pady=(0, 10))
        tk.Label(ctrl2, text='UID', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(ctrl2, textvariable=self._combatant_var, width=13).pack(side='left', padx=(6, 8))
        tk.Label(ctrl2, text='Skill', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(ctrl2, textvariable=self._skill_var, width=13).pack(side='left', padx=(6, 8))
        action_button(ctrl2, '过滤', self.filter).pack(side='left', padx=(0, 6))
        action_button(ctrl2, '复制', self.copy).pack(side='left', padx=(0, 6))
        action_button(ctrl2, '返回', self.back).pack(side='left', padx=(0, 6))

        # Status bar
        tk.Label(body, textvariable=self._status_var, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(fill='x', padx=14, pady=(0, 6))

        # Two-column layout: main (left scrollable) + sidebar (right)
        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=14, pady=(0, 14))

        # Right sidebar
        side_card, side_inner = rounded_panel(outer, bg=_SAO_PANEL_BODY_BG,
                                              border=_SAO_PANEL_BORDER, radius=10, pad=12)
        side_card.configure(width=260)
        side_card.pack(side='right', fill='y', padx=(12, 0))
        self._side = side_inner

        # Main area (left): scrollable content
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
        _refresh_panel_palette()
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        refs = _mapping_items(status.get('timeline_refs'))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        cid = str(status.get('combatant_id') or self._combatant_var.get() or '')
        sid = str(status.get('skill_id') or self._skill_var.get() or '')
        if cid and self._combatant_var.get() != cid:
            self._combatant_var.set(cid)
        if sid and self._skill_var.get() != sid:
            self._skill_var.set(sid)
        casts = _finite_int(status.get('casts'), 0, lo=0)
        hits = _finite_int(status.get('hits'), 0, lo=0)
        self._summary_var.set(f"{summary.get('name') or sid or 'NONE'} · {casts} CASTS · {hits} HITS")
        # Minimal status text (webref shows no status bar)
        self._status_var.set('')
        if hasattr(self, '_badge_frame_sd'):
            for child in list(self._badge_frame_sd.winfo_children()):
                child.destroy()
            badge_text = 'READY' if status.get('ok', True) and not list(status.get('errors') or []) else 'ERROR'
            badge_kind = 'ok' if badge_text == 'READY' else 'danger'
            status_badge(self._badge_frame_sd, badge_text, kind=badge_kind).pack(side='left')
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_side(status)
        if not summary:
            self._render_empty()
            return
        self._render_skill_label(summary)
        self._render_metrics(status, summary)
        self._render_hitlog(refs)

    # ── Sidebar ──────────────────────────────────────────────────────────
    def _render_side(self, status: Mapping[str, Any]) -> None:
        side = getattr(self, '_side', None)
        if side is None:
            return
        for child in list(side.winfo_children()):
            child.destroy()
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        pad = tk.Frame(side, bg=_SAO_PANEL_BODY_BG)
        pad.pack(fill='both', expand=True)
        tk.Label(pad, text='SKILLS 技能', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_sao_font(9, True), anchor='w').pack(fill='x', pady=(0, 8))
        # Skill cards: show current skill as a card with damage pill (webref style)
        skill_name = str(summary.get('name') or status.get('skill_id') or '-')
        skill_amount = _finite_float(summary.get('amount') or summary.get('damage'), 0.0)
        if skill_name and skill_name != '-':
            self._render_skill_card(pad, skill_name, skill_amount, active=True)
        errors = list(status.get('errors') or [])
        if errors:
            tk.Frame(pad, bg=_SAO_PANEL_BORDER, height=1).pack(fill='x', pady=(12, 8))
            tk.Label(pad, text='ERRORS', bg=_SAO_PANEL_BODY_BG, fg=_pc('danger', '#ef684e'),
                     font=get_sao_font(9, True), anchor='w').pack(fill='x')
            for err in errors[:5]:
                tk.Label(pad, text=str(err), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                         font=get_cjk_font(8), anchor='w', wraplength=220).pack(fill='x', pady=1)

    def _render_skill_card(self, parent: tk.Frame, name: str, amount: float, *, active: bool = False) -> None:
        # Render a sidebar skill card row with name and damage pill (webref style).
        card_bg = _pc('card_bg_alt', _SAO_PANEL_HEADER_BG) if active else _SAO_PANEL_BODY_BG
        card = tk.Frame(parent, bg=card_bg, highlightthickness=1,
                        highlightbackground=_SAO_PANEL_BORDER)
        card.pack(fill='x', pady=2)
        tk.Label(card, text=name, bg=card_bg, fg=_SAO_PANEL_VALUE_FG,
                 font=get_cjk_font(10), anchor='w').pack(side='left', padx=8, pady=6)
        pill_text = self._fmt(amount)
        status_badge(card, pill_text, kind='gold').pack(side='right', padx=8, pady=6)

    # ── Empty state ──────────────────────────────────────────────────────
    def _render_empty(self) -> None:
        if self._rows is None:
            return
        empty_state(self._rows, '请输入 combatant UID 与 skill ID',
                    '可从成员钻取或 DPS 技能行中选择。').pack(fill='x', pady=8, padx=4)

    # ── Skill name label ─────────────────────────────────────────────────
    def _render_skill_label(self, summary: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        skill_name = str(summary.get('name') or '-')
        name_frame = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        name_frame.pack(fill='x', padx=4, pady=(0, 6))
        tk.Label(name_frame, text=skill_name, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                 font=get_sao_font(13, True), anchor='w').pack(side='left')

    # ── Metric tiles ─────────────────────────────────────────────────────
    def _render_metrics(self, status: Mapping[str, Any], summary: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        amount = _finite_float(summary.get('amount'), 0.0)
        hits = _finite_int(status.get('hits'), 0, lo=0)
        crit_rate = _finite_float(status.get('crit_rate'), 0.0, lo=0.0, hi=1.0)
        avg_hit = amount / hits if hits > 0 else 0.0
        kind = str(summary.get('kind') or 'damage')
        amount_label = '总治疗' if kind == 'heal' else '总伤害'
        amount_accent = 'ok' if kind == 'heal' else 'gold'
        crit_pct = int(round(crit_rate * 100))
        items = (
            (amount_label, self._fmt(amount), '', amount_accent),
            ('命中', str(hits), '', 'cyan'),
            ('暴击率', f'{crit_pct}%', '', 'danger' if crit_rate >= 0.5 else 'cyan'),
            ('单次均伤', self._fmt(avg_hit), '', 'gold'),
        )
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        cols = 3
        for col in range(cols):
            grid.columnconfigure(col, weight=1, uniform='kpi')
        for idx, (label, value, sub, accent) in enumerate(items):
            metric_tile(grid, label, value, sub=str(sub), accent=accent).grid(
                row=idx // cols, column=idx % cols, sticky='nsew', padx=3, pady=3)

    # ── Hit log section (collapsible section_card) ───────────────────────
    def _render_hitlog(self, refs: list[Mapping[str, Any]]) -> None:
        if self._rows is None:
            return
        ref_count = len(refs)
        box = section_card(self._rows, '逐次命中 Hit log',
                           subtitle='', badge=str(ref_count), accent='cyan')
        box.pack(fill='x', padx=4, pady=(0, 8))
        if not refs:
            tk.Label(box, text='暂无命中记录', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(10), pady=20).pack(fill='x')
            return
        # Determine encounter base time for relative offsets
        base_ms = 0
        for r in refs:
            t = 0
            try:
                t = int(r.get('time_ms') or 0)
            except Exception:
                pass
            if t > 0:
                if base_ms == 0 or t < base_ms:
                    base_ms = t
        # Table header
        table = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        table.pack(fill='x', padx=SP_SM, pady=(SP_SM, 0))
        hdr_bg = _pc('header_bg', _SAO_PANEL_HEADER_BG)
        hdr = tk.Frame(table, bg=hdr_bg)
        hdr.pack(fill='x')
        for text, w in (('时间', 10), ('目标', 14), ('伤害', 12), ('类型', 10), ('暴击', 6)):
            tk.Label(hdr, text=text, width=w, anchor='w', bg=hdr_bg, fg=_SAO_PANEL_GOLD,
                     font=get_cjk_font(9, True)).pack(side='left', padx=3, pady=5)
        # Table rows
        for idx, ref in enumerate(refs[:80]):
            ref_id = str(ref.get('id') or f"ref:{idx}:{ref.get('time_ms')}")
            expanded = ref_id in self._expanded_refs
            row_bg = _pc('card_bg_alt', _SAO_PANEL_HEADER_BG) if idx % 2 else _SAO_PANEL_BODY_BG
            row = tk.Frame(table, bg=row_bg, cursor='hand2')
            row.pack(fill='x', pady=1)
            caret = tk.Label(row, text=('▾' if expanded else '▸'), bg=row_bg, fg=_SAO_PANEL_GOLD,
                             font=get_cjk_font(9), cursor='hand2')
            caret.pack(side='left', padx=(4, 0), pady=4)
            caret.bind('<Button-1>', lambda _e, rid=ref_id: self._toggle_ref(rid))
            # Determine if this hit is a crit based on label/payload
            payload = ref.get('payload') if isinstance(ref.get('payload'), Mapping) else {}
            is_crit = bool(payload.get('crit') or payload.get('is_crit') or payload.get('critical'))
            # Type column: crit type label (webref: 暴击/普通)
            type_text = '暴击' if is_crit else '普通'
            # Crit indicator column (webref: checkmark/dash)
            crit_text = '✓' if is_crit else '—'
            # Relative time MM:SS.mmm
            rel_time = self._fmt_rel_ms(ref.get('time_ms'), base_ms)
            # Damage with comma separators
            dmg_val = _finite_float(ref.get('value'), 0.0)
            dmg_text = f'{int(dmg_val):,}'
            cols = (
                (rel_time, 10, _SAO_PANEL_GOLD),
                (str(ref.get('label') or ref.get('target') or '-'), 14, _SAO_PANEL_VALUE_FG),
                (dmg_text, 12, _SAO_PANEL_VALUE_FG),
                (type_text, 10, _SAO_PANEL_LABEL_FG),
                (crit_text, 6, _pc('danger', '#ef684e') if is_crit else _SAO_PANEL_LABEL_FG),
            )
            for text, w, fg in cols:
                lbl = tk.Label(row, text=text, width=w, anchor='w', bg=row_bg, fg=fg,
                               font=get_cjk_font(9), cursor='hand2')
                lbl.pack(side='left', padx=3, pady=4)
                lbl.bind('<Button-1>', lambda _e, rid=ref_id: self._toggle_ref(rid))
            row.bind('<Button-1>', lambda _e, rid=ref_id: self._toggle_ref(rid))
            if expanded:
                self._render_ref_payload(ref, parent=table)
        hidden = len(refs) - 80
        if hidden > 0:
            more_indicator(table, hidden).pack(fill='x', padx=4, pady=(2, 0))

    def _toggle_ref(self, ref_id: str) -> None:
        if ref_id in self._expanded_refs:
            self._expanded_refs.remove(ref_id)
        else:
            self._expanded_refs.add(ref_id)
        self._last_sig = ""
        self._render_status(self._last_status)

    def _render_ref_payload(self, ref: Mapping[str, Any], *, parent: Optional[tk.Frame] = None) -> None:
        target = parent if parent is not None else self._rows
        if target is None:
            return
        payload = ref.get('payload') if isinstance(ref.get('payload'), Mapping) else ref
        box_bg = _pc('header_bg', '#0f1720')
        box = tk.Frame(target, bg=box_bg, highlightthickness=1, highlightbackground=_SAO_PANEL_GOLD)
        box.pack(fill='x', padx=12, pady=(0, 4))
        text = json.dumps(payload, ensure_ascii=False, indent=2, default=str)
        tk.Label(box, text=text, bg=box_bg, fg=_pc('value_fg', '#d7f7ff'), font=get_sao_font(9),
                 anchor='w', justify='left', wraplength=560).pack(fill='x', padx=8, pady=6)

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
    def _fmt_rel_ms(time_ms: Any, base_ms: int) -> str:
        # Format time_ms as relative offset MM:SS.mmm from base_ms.
        try:
            ms = int(time_ms or 0)
        except Exception:
            ms = 0
        if ms <= 0:
            return '--'
        delta = max(0, ms - base_ms)
        total_s, frac_ms = divmod(delta, 1000)
        minutes, seconds = divmod(int(total_s), 60)
        return f'{minutes:02d}:{seconds:02d}.{frac_ms:03d}'

    def _signature(self, status: Mapping[str, Any]) -> str:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        refs = []
        for ref in _mapping_items(status.get('timeline_refs'))[:80]:
            refs.append((ref.get('id'), ref.get('time_ms'), ref.get('topic'), ref.get('label'), ref.get('value'), ref.get('payload') if str(ref.get('id')) in self._expanded_refs else None))
        filters = status.get('filters') if isinstance(status.get('filters'), Mapping) else {}
        return repr((
            status.get('combatant_id'),
            status.get('skill_id'),
            summary.get('name'),
            summary.get('kind'),
            summary.get('amount'),
            summary.get('damage'),
            summary.get('heal'),
            status.get('casts'),
            status.get('hits'),
            status.get('crit_rate'),
            filters.get('query'),
            tuple(sorted(self._expanded_refs)),
            refs,
        ))
