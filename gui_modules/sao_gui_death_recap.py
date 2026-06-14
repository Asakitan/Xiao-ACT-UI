# -*- coding: utf-8 -*-
"""Entity-mode ACT death recap panel."""

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import act_death_recap_copy, act_death_recap_status
from gui_modules.sao_panel_components import (
    keep_canvas_scroll,
    action_button,
    aggregate_row,
    empty_state,
    fmt_clock,
    fmt_dur,
    fmt_signed,
    metric_tile,
    sao_entry,
    sao_scrollbar,
    section_card,
    status_badge,
    topic_cn,
)
from utils.sao_sound import get_sao_font, get_cjk_font
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


def _mapping_rows(value: Any) -> list[Mapping[str, Any]]:
    if not isinstance(value, list):
        return []
    return [row for row in value if isinstance(row, Mapping)]


class DeathRecapPanel:
    """Compact death recap window for Entity/Tk."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="DEATH RECAP: --")
        self._status_var = tk.StringVar(value="Ready")
        self._entity_var = tk.StringVar(value="")
        self._window_var = tk.StringVar(value="8")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[Any, ...] = ()
        self._last_rows_sig = ""
        self._expanded_rows: set[str] = set()

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
            window_s = float(self._window_var.get() or 8.0)
        except Exception:
            window_s = 8.0
        entity_id = self._entity_var.get() or None
        request_key = (entity_id or "", window_s)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_death_recap_status(
                self.owner,
                limit=80,
                window_s=window_s,
                entity_id=entity_id,
            )
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "death": None, "rows": [], "summary": {}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def copy_json(self) -> Dict[str, Any]:
        try:
            window_s = float(self._window_var.get() or 8.0)
        except Exception:
            window_s = 8.0
        try:
            result = act_death_recap_copy(self.owner, limit=80, window_s=window_s, entity_id=self._entity_var.get() or None)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "text": json.dumps(self._last_status, ensure_ascii=False, indent=2)}
        text = str(result.get('text') or json.dumps(self._last_status, ensure_ascii=False, indent=2))
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Death recap copied to clipboard')
        except Exception as exc:
            self._status_var.set(str(exc))
            result = dict(result)
            result.update({"ok": False, "message": str(exc)})
        return dict(result or {})

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Death Recap')
        win.geometry('960x862+250+180')
        win.minsize(680, 400)
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
        header = _sao_panel_header(win, 'ACT DEATH RECAP', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)
        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT DEATH', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='DEATH RECAP 死亡回放', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))
        self._badge_frame_dr = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        self._badge_frame_dr.pack(side='left', padx=(12, 0), anchor='n', pady=8)
        for label, cmd in (('刷新', self.refresh), ('×', self.hide)):
            action_button(toolbar, label, cmd, kind='gold').pack(side='right', padx=(6, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(control, text='Entity', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(control, textvariable=self._entity_var, width=16).pack(side='left', padx=(6, 10))
        tk.Label(control, text='Window s', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(control, textvariable=self._window_var, width=7).pack(side='left', padx=(6, 10))
        tk.Button(control, text='应用', command=self.refresh).pack(side='left')

        tk.Label(body, textvariable=self._status_var, anchor='w', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(fill='x', padx=12, pady=(0, 6))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=12, pady=(0, 12))
        canvas = tk.Canvas(outer, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(outer, canvas.yview)
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
        rows = _mapping_rows(status.get('rows'))
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        death = status.get('death') if isinstance(status.get('death'), Mapping) else {}
        death_count = int(summary.get('death_events') or len([r for r in rows if str(r.get('kind') or r.get('topic') or '') == 'death']) or 0)
        if hasattr(self, '_badge_frame_dr'):
            for child in list(self._badge_frame_dr.winfo_children()):
                child.destroy()
            from gui_modules.sao_panel_components import status_badge as _sb
            _sb(self._badge_frame_dr, f'{death_count} DEATHS' if death_count else 'NO DEATHS',
                kind='danger' if death_count else 'ok').pack(side='left')
        self._status_var.set(
            f"death={death.get('name') or death.get('entity_id') or '-'} · encounter={status.get('encounter_id') or 'live'}"
        )
        if self._rows is None:
            return
        sig = self._render_signature(status, rows, self._expanded_rows)
        if sig == self._last_rows_sig:
            return
        self._last_rows_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_metrics(status, rows, summary, death)
        if not rows:
            empty_state(self._rows, '暂无死亡回放', '等待 death/is_dead/hp=0 事件。').pack(fill='x', pady=8, padx=4)
            return
        box = section_card(self._rows, '死亡前后事件摘要', subtitle='默认显示 compact rows；点击单行展开 payload/raw detail。', badge=str(len(rows)))
        box.pack(fill='x', padx=4, pady=(0, 8))
        body = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        body.pack(fill='x', padx=8, pady=8)
        for row in rows[:120]:
            self._render_row(row, parent=body)

    def _render_metrics(self, status: Mapping[str, Any], rows: list[Any], summary: Mapping[str, Any], death: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        window = status.get('window') if isinstance(status.get('window'), Mapping) else {}
        grid = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x', padx=4, pady=(0, 8))
        items = (
            ('Incoming', self._fmt(summary.get('incoming_damage')), f"{_finite_int(summary.get('death_events'), 0, lo=0)} death", 'danger'),
            ('Healing', self._fmt(summary.get('healing')), f"shield {self._fmt(summary.get('shield'))}", 'heal'),
            ('Rows', len(rows), f"window {fmt_dur(window.get('before_ms'))}", 'cyan'),
            ('Death', death.get('name') or death.get('entity_id') or '-', f"@ {fmt_clock(death.get('time_ms'))}", 'gold'),
        )
        for label, value, sub, accent in items:
            metric_tile(grid, label, value, sub=str(sub), accent=accent).pack(side='left', fill='x', expand=True, padx=3)
        badges = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        badges.pack(fill='x', padx=4, pady=(0, 8))
        status_badge(badges, f"ENCOUNTER {status.get('encounter_id') or 'live'}", kind='cyan').pack(side='left', padx=(0, 6))
        status_badge(badges, f"ERRORS {len(status.get('errors') or [])}", kind='danger' if status.get('errors') else 'cyan').pack(side='left', padx=(0, 6))

    def _render_row(self, row: Mapping[str, Any], parent: Optional[tk.Misc] = None) -> None:
        parent = parent or self._rows
        if parent is None:
            return
        row_id = str(row.get('id') or f"{row.get('kind')}:{row.get('time_ms')}:{row.get('index')}")
        open_row = row_id in self._expanded_rows
        rel = _finite_int(row.get('relative_ms'), 0)
        detail = f"{row.get('actor') or '-'} -> {row.get('target') or '-'} · {row.get('label') or ''}"
        kind = str(row.get('kind') or row.get('topic') or 'event')
        accent = 'danger' if row.get('is_death') or kind == 'incoming_damage' else ('heal' if kind == 'healing' else 'gold')
        aggregate_row(
            parent,
            title=f"{fmt_signed(rel)} · {topic_cn(kind)}",
            meta=detail,
            value=self._fmt(row.get('amount')),
            ratio=1.0 if row.get('is_death') else 0.35,
            accent=accent,
            command=lambda key=row_id: self._toggle_row(key),
            expanded=open_row,
        ).pack(fill='x', pady=2)
        if open_row:
            payload = json.dumps(row.get('payload') or {}, ensure_ascii=False, default=str)
            tk.Label(parent, text=payload, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, anchor='w', justify='left', wraplength=740, font=get_cjk_font(8)).pack(fill='x', padx=22, pady=(0, 6))

    def _toggle_row(self, row_id: str) -> None:
        if row_id in self._expanded_rows:
            self._expanded_rows.remove(row_id)
        else:
            self._expanded_rows.add(row_id)
        self._last_rows_sig = ""
        self._render_status(self._last_status)

    @staticmethod
    def _render_signature(status: Mapping[str, Any], rows: list[Any], expanded_rows: set[str]) -> str:
        summary = status.get('summary') if isinstance(status.get('summary'), Mapping) else {}
        death = status.get('death') if isinstance(status.get('death'), Mapping) else {}
        window = status.get('window') if isinstance(status.get('window'), Mapping) else {}
        expanded = {str(row_id) for row_id in expanded_rows}
        row_parts = []
        for row in rows[:120]:
            if not isinstance(row, Mapping):
                continue
            row_id = str(row.get('id') or f"{row.get('kind')}:{row.get('time_ms')}:{row.get('index')}")
            row_parts.append((
                row_id,
                row.get('time_ms'),
                row.get('relative_ms'),
                row.get('kind'),
                row.get('topic'),
                row.get('amount'),
                row.get('actor'),
                row.get('target'),
                row.get('label'),
                bool(row.get('is_death')),
                row.get('payload') if row_id in expanded else None,
            ))
        return repr((
            len(rows),
            summary.get('incoming_damage'),
            summary.get('healing'),
            summary.get('shield'),
            summary.get('death_events'),
            death.get('name'),
            death.get('entity_id'),
            death.get('time_ms'),
            status.get('encounter_id'),
            window.get('before_ms'),
            tuple(status.get('errors') or ()),
            tuple(sorted(expanded)),
            tuple(row_parts),
        ))

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
