# -*- coding: utf-8 -*-
"""Entity-mode ACT report/export panel."""

from __future__ import annotations

import json
import time
import tkinter as tk
from tkinter import filedialog
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import (
    act_history_delete,
    act_history_load,
    act_offline_import_file,
    act_report_copy,
    act_report_export,
    act_report_status,
    act_mini_parse_copy,
    act_selective_parsing_status,
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
    _SAO_PANEL_SEP,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
)


class ReportExportPanel:
    """SAO-styled Toplevel for ACT report preview and export."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._history: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="NO REPORT")
        self._status_var = tk.StringVar(value="Ready")
        self._format_var = tk.StringVar(value="json")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_render_sig = ""
        self._last_history_sig = ""

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
        self._history = None
        self._reset_render_cache()

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        now = time.time()
        if self._last_status and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_report_status(self.owner, limit=20, fmt=self._format_var.get())
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "preview": {}, "history": [], "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._render_status(self._last_status)
        return self._last_status

    def export_json(self) -> Dict[str, Any]:
        self._format_var.set('json')
        return self._export('json')

    def export_csv(self) -> Dict[str, Any]:
        self._format_var.set('csv')
        return self._export('csv')

    def export_html(self) -> Dict[str, Any]:
        self._format_var.set('html')
        return self._export('html')

    def export_xml(self) -> Dict[str, Any]:
        self._format_var.set('xml')
        return self._export('xml')

    def export_xml_gzip(self) -> Dict[str, Any]:
        self._format_var.set('xml.gz')
        return self._export('xml.gz')

    def export_xml_zip(self) -> Dict[str, Any]:
        self._format_var.set('xml.zip')
        return self._export('xml.zip')

    def copy_snapshot(self) -> Dict[str, Any]:
        try:
            result = act_report_copy(self.owner, fmt=self._format_var.get())
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "text": json.dumps(self._last_status, ensure_ascii=False, indent=2)}
        text = str(result.get('text') or json.dumps(self._last_status, ensure_ascii=False, indent=2))
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Report payload copied to clipboard')
        except Exception as exc:
            self._status_var.set(str(exc))
            result = dict(result)
            result.update({"ok": False, "message": str(exc)})
        return dict(result or {})

    def copy_mini_parse(self) -> Dict[str, Any]:
        try:
            result = act_mini_parse_copy(self.owner, formatter_id='summary_table')
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "text": ""}
        text = str(result.get('text') or '')
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Mini-Parse copied to clipboard')
            result = dict(result)
            result['copied'] = True
        except Exception as exc:
            self._status_var.set(str(exc))
            result = dict(result)
            result.update({"ok": False, "message": str(exc)})
        return dict(result or {})

    def _export(self, fmt: str) -> Dict[str, Any]:
        try:
            result = act_report_export(self.owner, fmt=fmt)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        if result.get('ok'):
            self._status_var.set(f"Exported: {result.get('path') or ''}")
        else:
            self._status_var.set(str(result.get('message') or 'Export failed'))
        self._last_status = dict(result or {})
        self._last_refresh_at = time.time()
        self._render_status(self._last_status)
        return self._last_status

    def load_history(self, index: int) -> Dict[str, Any]:
        try:
            result = act_history_load(self.owner, index=int(index or 0), show=True)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Loaded' if result.get('ok') else 'Load failed')))
        self._last_refresh_at = 0.0
        self.refresh()
        return dict(result or {})

    def delete_history(self, index: int) -> Dict[str, Any]:
        try:
            result = act_history_delete(self.owner, index=int(index or 0))
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Deleted' if result.get('ok') else 'Delete failed')))
        self._last_refresh_at = 0.0
        self._last_history_sig = ""
        self.refresh()
        return dict(result or {})

    def clear_history(self) -> Dict[str, Any]:
        try:
            result = act_history_delete(self.owner, clear=True)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Cleared' if result.get('ok') else 'Clear failed')))
        self._last_refresh_at = 0.0
        self._last_history_sig = ""
        self.refresh()
        return dict(result or {})

    def import_offline_file(self, path: str | None = None) -> Dict[str, Any]:
        selected = str(path or '').strip()
        if not selected:
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
                result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
                self._status_var.set(str(exc))
                return result
        if not selected:
            result = {"ok": False, "cancelled": True, "message": "No file selected", "errors": []}
            self._status_var.set('Import cancelled')
            return result
        try:
            result = act_offline_import_file(self.owner, selected, persist=True, show=True)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Imported' if result.get('ok') else 'Import failed')))
        self._last_refresh_at = 0.0
        self._last_history_sig = ""
        self._last_render_sig = ""
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
        win.title('SAO ACT Report Export')
        win.geometry('840x560+230+165')
        win.minsize(680, 430)
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
        header = _sao_panel_header(win, 'ACT REPORT / EXPORT', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'REPORT SDK').pack(side='left')
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(side='left', padx=(12, 0))
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('导入 Import', self.import_offline_file),
            ('导出 JSON', self.export_json),
            ('导出 CSV', self.export_csv),
            ('导出 HTML', self.export_html),
            ('导出 XML', self.export_xml),
            ('导出 XML.GZ', self.export_xml_gzip),
            ('导出 XML.ZIP', self.export_xml_zip),
            ('Mini Copy', self.copy_mini_parse),
            ('复制 Copy', self.copy_snapshot),
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
        left = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG)
        left.pack(side='left', fill='both', expand=True, padx=(0, 10))
        right = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG, width=270)
        right.pack(side='right', fill='y')
        right.pack_propagate(False)

        canvas = tk.Canvas(left, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = tk.Scrollbar(left, orient='vertical', command=canvas.yview)
        self._rows = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')

        tk.Label(
            right,
            text='HISTORY',
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(fill='x', pady=(2, 8))
        tk.Button(
            right,
            text='清空历史 Clear All',
            command=self.clear_history,
            bg=_SAO_PANEL_HEADER_BG,
            fg=_SAO_PANEL_HEADER_FG,
            activebackground=_SAO_PANEL_ACCENT,
            activeforeground='white',
            relief='flat',
            bd=0,
            padx=8,
            pady=4,
        ).pack(fill='x', pady=(0, 8))
        self._history = tk.Frame(right, bg=_SAO_PANEL_BODY_BG)
        self._history.pack(fill='both', expand=True)
        win.protocol('WM_DELETE_WINDOW', self.hide)
        self._reset_render_cache()

    def _reset_render_cache(self) -> None:
        self._last_render_sig = ""
        self._last_history_sig = ""

    def _render_status(self, status: Mapping[str, Any]) -> None:
        preview = status.get('preview') or {}
        history = status.get('history') or []
        errors = status.get('errors') or []
        fmt = str(status.get('selected_format') or self._format_var.get() or 'json').upper()
        self._summary_var.set(f"{fmt} · {int(preview.get('total_damage') or 0)} DMG")
        try:
            selective = act_selective_parsing_status(self.owner)
        except Exception:
            selective = {}
        selective_hint = ' · Selective ON' if selective.get('enabled') else ''
        if status.get('path'):
            self._status_var.set(f"Exported: {status.get('path')}")
        else:
            self._status_var.set(str(status.get('message') or ('OK' if status.get('ok') else f'errors={len(errors)}')) + selective_hint)
        render_sig = json.dumps({
            'preview': preview,
            'errors': errors,
            'fmt': fmt,
        }, ensure_ascii=False, sort_keys=True, default=str)
        if self._rows is not None and render_sig != self._last_render_sig:
            self._last_render_sig = render_sig
            for child in list(self._rows.winfo_children()):
                child.destroy()
            self._render_preview(preview, errors)
        history_sig = json.dumps(history[:12], ensure_ascii=False, sort_keys=True, default=str)
        if self._history is not None and history_sig != self._last_history_sig:
            self._last_history_sig = history_sig
            for child in list(self._history.winfo_children()):
                child.destroy()
            self._render_history(history)

    def _render_preview(self, preview: Mapping[str, Any], errors: list[Any]) -> None:
        if self._rows is None:
            return
        if errors and not preview.get('top_rows'):
            self._empty_box('\n'.join(str(err) for err in errors))
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=6, padx=4)
        title = str(preview.get('title') or 'Last Encounter')
        tk.Label(
            box,
            text=title,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_VALUE_FG,
            anchor='w',
            font=('Segoe UI', 12, 'bold'),
            padx=10,
            pady=8,
        ).pack(fill='x')
        metrics = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        metrics.pack(fill='x', padx=10, pady=(0, 8))
        for label, value in (
            ('Damage', preview.get('total_damage') or 0),
            ('DPS', preview.get('total_dps') or 0),
            ('Heal', preview.get('total_heal') or 0),
            ('Duration', f"{preview.get('elapsed_s') or 0}s"),
        ):
            cell = tk.Frame(metrics, bg='#07111c', highlightthickness=1, highlightbackground=_SAO_PANEL_SEP)
            cell.pack(side='left', fill='x', expand=True, padx=(0, 6))
            tk.Label(cell, text=label, bg='#07111c', fg=_SAO_PANEL_LABEL_FG, font=('Segoe UI', 8), pady=3).pack(fill='x')
            tk.Label(cell, text=str(value), bg='#07111c', fg=_SAO_PANEL_GOLD, font=('Segoe UI', 11, 'bold'), pady=4).pack(fill='x')
        rows = list(preview.get('top_rows') or [])
        if not rows:
            self._empty_box('暂无战斗成员数据 / No combatants')
            return
        for row in rows[:12]:
            self._render_row(row)

    def _render_row(self, row: Mapping[str, Any]) -> None:
        if self._rows is None:
            return
        card = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        card.pack(fill='x', pady=4, padx=4)
        tk.Label(
            card,
            text=f"#{row.get('rank') or '-'} {row.get('name') or row.get('uid') or '-'}",
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_VALUE_FG,
            anchor='w',
            font=('Segoe UI', 10, 'bold'),
            padx=9,
            pady=4,
        ).pack(fill='x')
        tk.Label(
            card,
            text=f"damage={row.get('damage') or 0} · dps={row.get('dps') or 0} · heal={row.get('heal') or 0}",
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            anchor='w',
            font=('Consolas', 9),
            padx=9,
            pady=(0, 6),
        ).pack(fill='x')

    def _render_history(self, history: list[Any]) -> None:
        if self._history is None:
            return
        if not history:
            tk.Label(
                self._history,
                text='暂无历史报告',
                bg=_SAO_PANEL_BODY_BG,
                fg=_SAO_PANEL_LABEL_FG,
                justify='center',
                font=('Segoe UI', 9),
                pady=20,
            ).pack(fill='x')
            return
        for idx, item in enumerate(history[:12]):
            if not isinstance(item, Mapping):
                continue
            history_index = int(item.get('_history_index') if item.get('_history_index') is not None else idx)
            box = tk.Frame(self._history, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            box.pack(fill='x', pady=4)
            tk.Label(
                box,
                text=f"{item.get('completed_local_time') or item.get('report_reason') or 'Encounter'}\nDMG {item.get('total_damage') or 0} · DPS {item.get('total_dps') or 0}",
                bg=_SAO_PANEL_BODY_BG,
                fg=_SAO_PANEL_LABEL_FG,
                anchor='w',
                justify='left',
                font=('Segoe UI', 9),
                padx=8,
                pady=7,
            ).pack(fill='x')
            actions = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
            actions.pack(fill='x', padx=6, pady=(0, 6))
            for label, command in (
                ('载入 Load', lambda i=history_index: self.load_history(i)),
                ('删除 Delete', lambda i=history_index: self.delete_history(i)),
            ):
                tk.Button(
                    actions,
                    text=label,
                    command=command,
                    bg=_SAO_PANEL_HEADER_BG,
                    fg=_SAO_PANEL_HEADER_FG,
                    activebackground=_SAO_PANEL_ACCENT,
                    activeforeground='white',
                    relief='flat',
                    bd=0,
                    padx=6,
                    pady=3,
                ).pack(side='left', fill='x', expand=True, padx=(0, 4))

    def _empty_box(self, message: str) -> None:
        if self._rows is None:
            return
        box = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(
            box,
            text=message,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=('Segoe UI', 10),
            pady=28,
        ).pack(fill='x')
