"""Entity-mode ACT offline import wizard panel."""

from __future__ import annotations

import json
import math
import os
import tkinter as tk
from tkinter import filedialog
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_history_load,
    act_offline_import_file,
    act_offline_import_status,
)
from gui_modules.sao_panel_components import (
    SP_SM,
    SP_MD,
    _pc,
    action_button,
    keep_canvas_scroll,
    metric_tile,
    sao_entry,
    sao_scrollbar,
    section_card,
    status_badge,
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
    _sao_panel_body,
    _sao_panel_header,
)


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    try:
        number = float(value)
    except Exception:
        number = float(default)
    if not math.isfinite(number):
        number = float(default)
    result = int(number)
    if lo is not None:
        result = max(lo, result)
    if hi is not None:
        result = min(hi, result)
    return result


_STEP_LABELS = ('① 选择文件', '② 解析预览', '③ 映射字段', '④ 导入')


class OfflineImportPanel:
    """SAO-styled standalone ACT offline import/history playback wizard."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._path_var = tk.StringVar(value="")
        self._summary_var = tk.StringVar(value="OFFLINE IMPORT: IDLE")
        self._status_var = tk.StringVar(value="Ready")
        self._last_status: Dict[str, Any] = {}
        self._last_rows_sig = ""
        self._current_step = 1  # 1-based step index for the wizard

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
        self._reset_render_cache()

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        try:
            status = act_offline_import_status(self.owner, history_limit=20)
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "status": "error", "history": {"encounters": []}, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        selected = str(self._last_status.get('selected_file') or '')
        if selected and not self._path_var.get():
            self._path_var.set(selected)
        self._render_status(self._last_status)
        return self._last_status

    def choose_file(self) -> Dict[str, Any]:
        try:
            selected = filedialog.askopenfilename(
                parent=self._win,
                title='Import ACT replay/report',
                filetypes=(
                    ('ACT replay/report', '*.json *.jsonl *.ndjson *.xml *.xml.gz *.xml.zip *.zip'),
                    ('SAO ACT XML report', '*.xml *.xml.gz *.xml.zip *.zip'),
                    ('JSON', '*.json'),
                    ('JSONL/NDJSON', '*.jsonl *.ndjson'),
                    ('All files', '*.*'),
                ),
            )
        except Exception as exc:
            self._status_var.set(str(exc))
            return {"ok": False, "message": str(exc), "errors": [str(exc)]}
        if not selected:
            self._status_var.set('Import cancelled')
            return {"ok": False, "cancelled": True, "message": "No file selected", "errors": []}
        self._path_var.set(str(selected))
        self._status_var.set(os.path.basename(str(selected)))
        return {"ok": True, "path": str(selected)}

    def import_file(self) -> Dict[str, Any]:
        path = str(self._path_var.get() or '').strip()
        if not path:
            self._status_var.set('No import path')
            return {"ok": False, "message": "No import path", "errors": ["No import path"]}
        try:
            result = act_offline_import_file(self.owner, path, persist=True, show=True)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Imported' if result.get('ok') else 'Import failed')))
        self._last_rows_sig = ""
        self.refresh()
        return dict(result or {})

    def load_history(self, index: int) -> Dict[str, Any]:
        history_index = _finite_int(index, 0, lo=0)
        try:
            result = act_history_load(self.owner, index=history_index, show=True)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Loaded' if result.get('ok') else 'Load failed')))
        self.refresh()
        return dict(result or {})

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Offline Import')
        win.geometry('960x862+245+175')
        win.minsize(700, 430)
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
        header = _sao_panel_header(win, 'ACT OFFLINE IMPORT', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        body_bg = _pc('body_bg', _SAO_PANEL_BODY_BG)

        # ── toolbar: dual-line title (webref) + step badge + nav buttons ──
        toolbar = tk.Frame(body, bg=body_bg)
        toolbar.pack(fill='x', padx=14, pady=(7, 10))

        title_box = tk.Frame(toolbar, bg=body_bg)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT IMPORT', bg=body_bg,
                 fg=_pc('gold', _SAO_PANEL_GOLD), font=get_sao_font(8, True),
                 anchor='w').pack(fill='x')
        tk.Label(title_box, text='OFFLINE IMPORT 离线导入', bg=body_bg,
                 fg=_pc('value_fg', _SAO_PANEL_VALUE_FG), font=get_sao_font(15, True),
                 anchor='w').pack(fill='x', pady=(1, 0))

        control = tk.Frame(toolbar, bg=body_bg)
        control.pack(side='right', anchor='n', pady=(10, 0))
        status_badge(control, f'STEP {self._current_step}/{len(_STEP_LABELS)}',
                     kind='gold').pack(side='left', padx=(0, SP_SM))
        action_button(control, '选择 Choose', self.choose_file, kind='normal').pack(
            side='left', padx=(0, SP_SM))
        action_button(control, '导入', self.import_file, kind='gold').pack(
            side='left', padx=(0, SP_SM))
        action_button(control, '刷新', self.refresh, kind='normal').pack(side='left')

        # ── step indicator row ──
        step_row = tk.Frame(body, bg=body_bg)
        step_row.pack(fill='x', padx=14, pady=(0, SP_SM))
        for idx, step_text in enumerate(_STEP_LABELS, start=1):
            is_active = (idx == self._current_step)
            fg = _pc('gold', _SAO_PANEL_GOLD) if is_active else _pc('label_fg', _SAO_PANEL_LABEL_FG)
            font = get_cjk_font(9, True) if is_active else get_cjk_font(9)
            lbl = tk.Label(step_row, text=step_text, bg=body_bg, fg=fg, font=font)
            lbl.pack(side='left', padx=(0, SP_MD))
            if is_active:
                # gold underline accent for active step
                accent_bar = tk.Frame(step_row, bg=_pc('gold', _SAO_PANEL_GOLD),
                                      height=2, width=0)
                # place under the label using the label's own frame
                accent_bar.place(in_=lbl, relx=0, rely=1.0, relwidth=1.0, height=2)

        # ── file drop zone card ──
        drop_sec = section_card(body, 'FILE 文件', subtitle='拖入 ACT 日志 / SQLite / JSON 文件', accent='cyan')
        drop_sec.pack(fill='x', padx=14, pady=(0, SP_SM))
        path_row = tk.Frame(drop_sec, bg=_pc('card_bg', _SAO_PANEL_BODY_BG))
        path_row.pack(fill='x', padx=SP_SM, pady=SP_SM)
        tk.Label(path_row, text='Path', bg=_pc('card_bg', _SAO_PANEL_BODY_BG),
                 fg=_pc('label_fg', _SAO_PANEL_LABEL_FG), font=get_cjk_font(9)).pack(side='left')
        sao_entry(path_row, textvariable=self._path_var).pack(
            side='left', fill='x', expand=True, padx=(SP_SM, 0))

        # ── tag pills row ──
        self._pill_row = tk.Frame(drop_sec, bg=_pc('card_bg', _SAO_PANEL_BODY_BG))
        self._pill_row.pack(fill='x', padx=SP_SM, pady=(0, SP_SM))

        # ── status line ──
        tk.Label(
            body,
            textvariable=self._status_var,
            anchor='w',
            bg=body_bg,
            fg=_pc('label_fg', _SAO_PANEL_LABEL_FG),
            font=get_cjk_font(9),
        ).pack(fill='x', padx=14, pady=(0, 6))

        # ── scrollable content area ──
        outer = tk.Frame(body, bg=body_bg)
        outer.pack(fill='both', expand=True, padx=14, pady=(0, 12))
        canvas = tk.Canvas(outer, bg=body_bg, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(outer, canvas.yview)
        self._rows = tk.Frame(canvas, bg=body_bg)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas
        win.protocol('WM_DELETE_WINDOW', self.hide)
        self._reset_render_cache()

    def _reset_render_cache(self) -> None:
        self._last_rows_sig = ""

    def _render_status(self, status: Mapping[str, Any]) -> None:
        last = status.get('last_result') if isinstance(status.get('last_result'), Mapping) else {}
        history = status.get('history') if isinstance(status.get('history'), Mapping) else {}
        encounters = list(history.get('encounters') or [])
        self._summary_var.set(
            f"{str(status.get('status') or 'idle').upper()} · {str(last.get('format') or '--')} · {len(encounters)} HISTORY"
        )
        errors = list(status.get('errors') or [])
        self._status_var.set(str(status.get('message') or 'Ready') + f" · errors={len(errors)}")
        # ── tag pills (format / row count / encoding) ──
        pill_row = getattr(self, '_pill_row', None)
        if pill_row is not None:
            for ch in list(pill_row.winfo_children()):
                ch.destroy()
            fmt = str(last.get('format') or '--').upper()
            event_count = _finite_int(last.get('event_count'), 0, lo=0)
            count_str = f'{event_count:,}' if event_count else '--'
            status_badge(pill_row, f'格式 {fmt}', kind='cyan').pack(side='left', padx=(0, SP_SM))
            status_badge(pill_row, f'{count_str} 行', kind='cyan').pack(side='left', padx=(0, SP_SM))
            status_badge(pill_row, '编码 UTF-8', kind='cyan').pack(side='left')
        if self._rows is None:
            return
        sig = self._rows_signature(status)
        if sig == self._last_rows_sig:
            return
        self._last_rows_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()
        self._render_import_preview(last)
        self._render_history(encounters)

    def _render_import_preview(self, last: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        card_bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
        label_fg = _pc('label_fg', _SAO_PANEL_LABEL_FG)
        value_fg = _pc('value_fg', _SAO_PANEL_VALUE_FG)

        preview = last.get('preview') if isinstance(last.get('preview'), Mapping) else {}
        report_obj = last.get('report') if isinstance(last.get('report'), Mapping) else {}
        encounter_id = preview.get('encounter_id') or report_obj.get('encounter_id') or ''
        event_count = _finite_int(last.get('event_count'), 0, lo=0)

        # ── metric tiles row ──
        tile_row = tk.Frame(self._rows, bg=_pc('body_bg', _SAO_PANEL_BODY_BG))
        tile_row.pack(fill='x', pady=(0, SP_SM))
        for col, (lbl, val, acc) in enumerate((
            ('ENCOUNTER', encounter_id or '--', 'gold'),
            ('FORMAT', str(last.get('format') or '--').upper(), 'cyan'),
            ('EVENTS', f'{event_count:,}' if event_count else '0', 'gold'),
            ('PERSISTED', 'YES' if last.get('persisted') else 'NO',
             'ok' if last.get('persisted') else 'danger'),
        )):
            tile_row.columnconfigure(col, weight=1)
            metric_tile(tile_row, lbl, val, accent=acc).grid(
                row=0, column=col, sticky='nsew', padx=(0 if col == 0 else SP_SM, 0))

        # ── preview table section ──
        sec = section_card(self._rows, 'PREVIEW · 前 4 行',
                           subtitle=str(last.get('importer') or ''),
                           badge=f'{event_count} events' if event_count else '',
                           accent='cyan')
        sec.pack(fill='x', pady=(0, SP_SM))

        values = (
            ('Encounter', encounter_id),
            ('Format', last.get('format') or '--'),
            ('Importer', last.get('importer') or '--'),
            ('Events', event_count),
            ('Persisted', 'YES' if last.get('persisted') else 'NO'),
        )
        for idx, (label, value) in enumerate(values):
            row_bg = _pc('card_bg_alt', _SAO_PANEL_HEADER_BG) if idx % 2 else card_bg
            row_frame = tk.Frame(sec, bg=row_bg)
            row_frame.pack(fill='x')
            tk.Label(row_frame, text=str(label), bg=row_bg, fg=label_fg,
                     font=get_cjk_font(9), anchor='w', width=12).pack(
                side='left', padx=(SP_SM, SP_SM), pady=2)
            tk.Label(row_frame, text=str(value), bg=row_bg, fg=value_fg,
                     font=get_cjk_font(9), anchor='w').pack(
                side='left', fill='x', expand=True, padx=(0, SP_SM), pady=2)

    def _render_history(self, encounters: list[Any]) -> None:
        if self._rows is None:
            return
        card_bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
        label_fg = _pc('label_fg', _SAO_PANEL_LABEL_FG)
        value_fg = _pc('value_fg', _SAO_PANEL_VALUE_FG)

        sec = section_card(self._rows, 'HISTORY PLAYBACK 历史回放',
                           badge=f'{len(encounters)}' if encounters else '',
                           accent='gold')
        sec.pack(fill='x', pady=(SP_SM, 0))

        if not encounters:
            tk.Label(sec, text='暂无历史报告', bg=card_bg,
                     fg=label_fg, font=get_cjk_font(9), pady=SP_MD).pack(fill='x')
            return
        for idx, item in enumerate(encounters[:20]):
            if not isinstance(item, Mapping):
                continue
            history_index = _finite_int(
                item.get('_history_index') if item.get('_history_index') is not None else idx,
                idx,
                lo=0,
            )
            row_bg = _pc('card_bg_alt', _SAO_PANEL_HEADER_BG) if idx % 2 else card_bg
            row_frame = tk.Frame(sec, bg=row_bg)
            row_frame.pack(fill='x')
            enc_id = str(item.get('encounter_id') or '#')
            dmg = _finite_int(item.get('total_damage'), 0, lo=0)
            ts = str(item.get('completed_local_time') or '')
            tk.Label(row_frame, text=enc_id, bg=row_bg, fg=value_fg,
                     font=get_cjk_font(9, True), anchor='w').pack(
                side='left', padx=(SP_SM, SP_SM), pady=4)
            tk.Label(row_frame, text=f'dmg={dmg:,}', bg=row_bg,
                     fg=_pc('gold', _SAO_PANEL_GOLD),
                     font=get_cjk_font(8), anchor='w').pack(
                side='left', padx=(0, SP_SM), pady=4)
            tk.Label(row_frame, text=ts, bg=row_bg, fg=label_fg,
                     font=get_cjk_font(8), anchor='w').pack(
                side='left', fill='x', expand=True, pady=4)
            action_button(row_frame, '加载 Load',
                          lambda i=history_index: self.load_history(i),
                          kind='normal').pack(
                side='right', padx=SP_SM, pady=3)

    @staticmethod
    def _rows_signature(status: Mapping[str, Any]) -> str:
        history = status.get('history') if isinstance(status.get('history'), Mapping) else {}
        encounters = history.get('encounters') if isinstance(history.get('encounters'), list) else []
        last = status.get('last_result') if isinstance(status.get('last_result'), Mapping) else {}
        preview = last.get('preview') if isinstance(last.get('preview'), Mapping) else {}
        report = last.get('report') if isinstance(last.get('report'), Mapping) else {}
        rendered_history = []
        for idx, item in enumerate(encounters[:20]):
            if not isinstance(item, Mapping):
                continue
            rendered_history.append({
                'index': item.get('_history_index') if item.get('_history_index') is not None else idx,
                'encounter_id': item.get('encounter_id'),
                'total_damage': item.get('total_damage'),
                'completed_local_time': item.get('completed_local_time'),
            })
        return json.dumps({
            'status': status.get('status'),
            'selected_file': status.get('selected_file'),
            'last': {
                'source_path': last.get('source_path'),
                'format': last.get('format'),
                'importer': last.get('importer'),
                'event_count': last.get('event_count'),
                'persisted': bool(last.get('persisted')),
                'preview_encounter_id': preview.get('encounter_id'),
                'report_encounter_id': report.get('encounter_id'),
            },
            'history': rendered_history,
        }, ensure_ascii=False, sort_keys=True, default=str)
