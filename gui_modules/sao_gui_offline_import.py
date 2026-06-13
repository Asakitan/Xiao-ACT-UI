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
    keep_canvas_scroll,
    sao_entry,
    sao_scrollbar,
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
        win.geometry('840x560+245+175')
        win.minsize(700, 430)
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
        header = _sao_panel_header(win, 'ACT OFFLINE IMPORT', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'IMPORT WIZARD').pack(side='left')
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('选择 Choose', self.choose_file),
            ('导入 Import', self.import_file),
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

        # 按钮先 pack — 窄窗下 summary 不挤按钮(后包者只分剩余空间)
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=get_cjk_font(10, True),
        ).pack(side='left', padx=(12, 0))
        path_row = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        path_row.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(path_row, text='Path', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(side='left')
        sao_entry(path_row, textvariable=self._path_var).pack(side='left', fill='x', expand=True, padx=(8, 0))

        tk.Label(
            body,
            textvariable=self._status_var,
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            font=get_cjk_font(9),
        ).pack(fill='x', padx=12, pady=(0, 6))

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
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=(0, 8), padx=4)
        tk.Label(box, text='IMPORT RESULT', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD, anchor='w', font=get_cjk_font(9, True)).pack(fill='x', padx=8, pady=(7, 3))
        preview = last.get('preview') if isinstance(last.get('preview'), Mapping) else {}
        report_obj = last.get('report') if isinstance(last.get('report'), Mapping) else {}
        encounter_id = preview.get('encounter_id') or report_obj.get('encounter_id') or ''
        values = (
            ('Encounter', encounter_id),
            ('Format', last.get('format') or '--'),
            ('Importer', last.get('importer') or '--'),
            ('Events', last.get('event_count') or 0),
            ('Persisted', 'YES' if last.get('persisted') else 'NO'),
        )
        for label, value in values:
            tk.Label(box, text=f"{label}: {value}", bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, anchor='w', font=get_cjk_font(9)).pack(fill='x', padx=8, pady=1)

    def _render_history(self, encounters: list[Any]) -> None:
        if self._rows is None:
            return
        title = tk.Frame(self._rows, bg=_SAO_PANEL_HEADER_BG)
        title.pack(fill='x', pady=(4, 2), padx=4)
        tk.Label(title, text='HISTORY PLAYBACK', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_GOLD, anchor='w', font=get_cjk_font(9, True)).pack(fill='x', padx=8, pady=5)
        if not encounters:
            tk.Label(self._rows, text='暂无历史报告', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, pady=24).pack(fill='x')
            return
        for idx, item in enumerate(encounters[:20]):
            if not isinstance(item, Mapping):
                continue
            history_index = _finite_int(
                item.get('_history_index') if item.get('_history_index') is not None else idx,
                idx,
                lo=0,
            )
            card = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            card.pack(fill='x', pady=3, padx=4)
            label = f"{item.get('encounter_id') or '#'} · dmg={item.get('total_damage') or 0} · {item.get('completed_local_time') or ''}"
            tk.Label(card, text=label, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, anchor='w', font=get_cjk_font(9)).pack(side='left', fill='x', expand=True, padx=8, pady=6)
            tk.Button(card, text='加载 Load', command=lambda i=history_index: self.load_history(i)).pack(side='right', padx=8, pady=5)

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
