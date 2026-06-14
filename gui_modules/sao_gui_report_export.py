# -*- coding: utf-8 -*-
"""Entity-mode ACT report/export panel."""

from __future__ import annotations

import json
import math
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
from gui_modules import sao_panel_components as components
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
    _SAO_PANEL_SEP,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _make_panel_close_button,
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
        self._last_request_key: tuple[Any, ...] = ()
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
        fmt = self._format_var.get()
        request_key = (fmt,)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_report_status(self.owner, limit=20, fmt=fmt)
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "preview": {}, "history": [], "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
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
        self._last_request_key = ()
        self._render_status(self._last_status)
        return self._last_status

    def load_history(self, index: int) -> Dict[str, Any]:
        history_index = _finite_int(index, 0, lo=0)
        try:
            result = act_history_load(self.owner, index=history_index, show=True)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Loaded' if result.get('ok') else 'Load failed')))
        self._last_refresh_at = 0.0
        self._last_request_key = ()
        self.refresh()
        return dict(result or {})

    def delete_history(self, index: int) -> Dict[str, Any]:
        history_index = _finite_int(index, 0, lo=0)
        try:
            result = act_history_delete(self.owner, index=history_index)
        except Exception as exc:
            result = {"ok": False, "message": str(exc), "errors": [str(exc)]}
        self._status_var.set(str(result.get('message') or ('Deleted' if result.get('ok') else 'Delete failed')))
        self._last_refresh_at = 0.0
        self._last_request_key = ()
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
        self._last_request_key = ()
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
        self._last_request_key = ()
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
        win.geometry('960x862+230+165')
        win.minsize(680, 430)
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
        header = _sao_panel_header(win, 'ACT REPORT', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='ACT REPORT', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='REPORT / EXPORT 报告导出', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))
        controls = tk.Frame(toolbar, bg=_SAO_PANEL_BODY_BG)
        controls.pack(side='right', anchor='s', pady=(0, 4))
        self._badge_frame = tk.Frame(controls, bg=_SAO_PANEL_BODY_BG)
        self._badge_frame.pack(side='left', padx=(0, 8))
        components.action_button(controls, '刷新', self.refresh, kind='gold').pack(side='left', padx=(0, 6))
        _make_panel_close_button(controls, self.hide, bg=_SAO_PANEL_BODY_BG, flat=True).pack(side='left', padx=(6, 0))

        fmt_row = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        fmt_row.pack(fill='x', padx=12, pady=(0, 8))
        for fmt_id, fmt_label, fmt_sub in (
            ('json', 'JSON', '完整事件+聚合'),
            ('csv', 'CSV', '逐行事件表'),
            ('markdown', 'Markdown', '战斗简报'),
            ('html', 'HTML', '可分享报告'),
        ):
            cmd = {'json': self.export_json, 'csv': self.export_csv, 'html': self.export_html}.get(fmt_id)
            tile = components.metric_tile(fmt_row, fmt_label, fmt_sub, accent='gold' if fmt_id == 'json' else 'cyan')
            tile.pack(side='left', fill='x', expand=True, padx=(0, 6))
            if cmd:
                tile.bind('<Button-1>', lambda _e, c=cmd: c(), add='+')
                tile.configure(cursor='hand2')

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=12, pady=(0, 12))
        left = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG)
        left.pack(side='left', fill='both', expand=True, padx=(0, 10))
        right = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG, width=270)
        right.pack(side='right', fill='y')
        right.pack_propagate(False)

        canvas = tk.Canvas(left, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = components.sao_scrollbar(left, canvas.yview)
        self._rows = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._rows.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._rows, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas

        tk.Label(
            right,
            text='OPTIONS 选项',
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=get_cjk_font(10, True),
        ).pack(fill='x', pady=(2, 8))
        opts_frame = tk.Frame(right, bg=_SAO_PANEL_BODY_BG)
        opts_frame.pack(fill='x', pady=(0, 12))
        self._opt_vars: dict[str, tk.BooleanVar] = {}
        for opt_key, opt_label, default in (
            ('include_raw', '含原始事件', True),
            ('include_aggregate', '含聚合', True),
            ('include_graph', '含图表', True),
            ('include_death', '含死亡回放', False),
            ('anonymize', '匿名化 UID', False),
        ):
            var = tk.BooleanVar(value=default)
            self._opt_vars[opt_key] = var
            tk.Checkbutton(opts_frame, text=opt_label, variable=var, bg=_SAO_PANEL_BODY_BG,
                           fg=_SAO_PANEL_VALUE_FG, selectcolor=_SAO_PANEL_HEADER_BG,
                           activebackground=_SAO_PANEL_BODY_BG, activeforeground=_SAO_PANEL_VALUE_FG,
                           font=get_cjk_font(9), anchor='w').pack(fill='x', pady=1)
        tk.Label(right, text='EXPORT', anchor='w', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_GOLD, font=get_cjk_font(10, True)).pack(fill='x', pady=(0, 8))
        components.action_button(right, '导出文件', self.export_json, kind='gold').pack(fill='x', pady=(0, 4))
        components.action_button(right, '复制到剪贴板', self.copy_snapshot).pack(fill='x', pady=(0, 12))
        self._history = tk.Frame(right, bg=_SAO_PANEL_BODY_BG)
        self._history.pack(fill='both', expand=True)
        win.protocol('WM_DELETE_WINDOW', self.hide)
        self._reset_render_cache()

    def _wire_clear_confirm(self, btn: tk.Button) -> None:
        """两段式确认：第一次点击进入「确认清空?」态，3 秒内再点才真正清空。"""
        state = {'armed': False, 'after': None}

        def _disarm() -> None:
            state['armed'] = False
            state['after'] = None
            try:
                btn.configure(text='清空历史 Clear All')
            except Exception:
                pass

        def _click() -> None:
            if state['armed']:
                if state['after'] is not None:
                    try:
                        btn.after_cancel(state['after'])
                    except Exception:
                        pass
                _disarm()
                self.clear_history()
                return
            state['armed'] = True
            try:
                btn.configure(text='确认清空? Confirm')
                state['after'] = btn.after(3000, _disarm)
            except Exception:
                _disarm()

        btn.configure(command=_click)

    def _reset_render_cache(self) -> None:
        self._last_render_sig = ""
        self._last_history_sig = ""

    def _render_status(self, status: Mapping[str, Any]) -> None:
        preview = status.get('preview') or {}
        history = status.get('history') or []
        errors = status.get('errors') or []
        fmt = str(status.get('selected_format') or self._format_var.get() or 'json').upper()
        for child in list(self._badge_frame.winfo_children()):
            child.destroy()
        components.status_badge(self._badge_frame, 'READY' if status.get('ok', True) else 'ERROR',
                                kind='ok' if status.get('ok', True) else 'danger').pack(side='left')
        try:
            selective = act_selective_parsing_status(self.owner)
        except Exception:
            selective = {}
        selective_hint = ' · Selective ON' if selective.get('enabled') else ''
        # 历史库异常透出 — 与 web 端 Storage=ERROR 对偶, 否则 DB 锁死时
        # 用户只看到空历史 + OK
        storage = status.get('storage_status') if isinstance(status.get('storage_status'), Mapping) else {}
        sqlite_info = storage.get('sqlite') if isinstance(storage.get('sqlite'), Mapping) else {}
        archive_info = storage.get('archive') if isinstance(storage.get('archive'), Mapping) else {}
        storage_err = str(sqlite_info.get('last_error') or archive_info.get('last_error') or '')
        storage_hint = f' · 历史库异常: {storage_err[:60]}' if storage_err else ''
        if status.get('path'):
            self._status_var.set(f"Exported: {status.get('path')}" + storage_hint)
        else:
            self._status_var.set(str(status.get('message') or ('OK' if status.get('ok') else f'errors={len(errors)}')) + selective_hint + storage_hint)
        render_sig = json.dumps({
            'preview': preview,
            'errors': errors,
            'fmt': fmt,
        }, ensure_ascii=False, sort_keys=True, default=str)
        if self._rows is not None and render_sig != self._last_render_sig:
            self._last_render_sig = render_sig
            components.keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
            for child in list(self._rows.winfo_children()):
                child.destroy()
            self._render_preview(preview, errors, fmt=fmt)
        history_sig = json.dumps(
            {'h': history[:12], 'p_title': preview.get('title'), 'p_events': preview.get('combatant_count')},
            ensure_ascii=False, sort_keys=True, default=str,
        )
        if self._history is not None and history_sig != self._last_history_sig:
            self._last_history_sig = history_sig
            for child in list(self._history.winfo_children()):
                child.destroy()
            self._render_history(history, preview=preview)

    def _render_preview(self, preview: Mapping[str, Any], errors: list[Any], *, fmt: str = "JSON") -> None:
        if self._rows is None:
            return
        if errors and not preview.get('top_rows'):
            self._empty_box('\n'.join(str(err) for err in errors))
            return
        # "PREVIEW - {FORMAT}" header
        tk.Label(
            self._rows,
            text=f'PREVIEW · {fmt}',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            anchor='w',
            font=get_cjk_font(10, True),
            padx=4,
        ).pack(fill='x', pady=(0, 4))
        # JSON code block preview
        rows = list(preview.get('top_rows') or [])
        preview_obj: dict[str, Any] = {"encounter": str(preview.get('title') or 'Last Encounter')}
        elapsed = preview.get('elapsed_s') or 0
        try:
            preview_obj["duration_s"] = round(float(elapsed))
        except Exception:
            preview_obj["duration_s"] = 0
        total_dps = _finite_int(preview.get('total_dps'), 0)
        if total_dps:
            preview_obj["party_dps"] = total_dps
        if rows:
            preview_obj["combatants"] = [
                {"name": str(r.get('name') or r.get('uid') or '-'),
                 "damage": _finite_int(r.get('damage'), 0),
                 "dps": _finite_int(r.get('dps'), 0)}
                for r in rows[:8]
            ]
        deaths = _finite_int(preview.get('deaths'), 0)
        if deaths:
            preview_obj["deaths"] = deaths
        event_count = _finite_int(preview.get('combatant_count'), 0)
        if event_count:
            preview_obj["events"] = event_count
        code_text = json.dumps(preview_obj, ensure_ascii=False, indent=2)
        code_box = tk.Frame(self._rows, bg=_SAO_PANEL_HEADER_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        code_box.pack(fill='x', pady=(0, 8), padx=4)
        tk.Label(
            code_box,
            text=code_text,
            bg=_SAO_PANEL_HEADER_BG,
            fg=_SAO_PANEL_VALUE_FG,
            anchor='nw',
            justify='left',
            font=('Consolas', 9),
            padx=14,
            pady=12,
            wraplength=560,
        ).pack(fill='x')

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
            font=get_cjk_font(10, True),
            padx=9,
            pady=4,
        ).pack(fill='x')
        dmg = _finite_int(row.get('damage'), 0)
        dps = _finite_int(row.get('dps'), 0)
        heal = _finite_int(row.get('heal'), 0)
        tk.Label(
            card,
            text=f"damage={dmg:,} · dps={dps:,} · heal={heal:,}",
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            anchor='w',
            font=('Consolas', 9),
            padx=9,
            pady=0,
        ).pack(fill='x', pady=(0, 6))

    def _render_history(self, history: list[Any], *, preview: Mapping[str, Any] | None = None) -> None:
        if self._history is None:
            return
        # Info card (Encounter / 事件数 / 大小估计) matching webref sidebar
        p = preview or {}
        title = str(p.get('title') or 'Last Encounter')
        event_count = _finite_int(p.get('combatant_count'), 0)
        # Rough size estimate from preview JSON
        try:
            size_bytes = len(json.dumps(p, ensure_ascii=False, default=str).encode('utf-8'))
            if size_bytes >= 1024 * 1024:
                size_text = f"~{size_bytes / (1024 * 1024):.1f} MB"
            elif size_bytes >= 1024:
                size_text = f"~{size_bytes // 1024} KB"
            else:
                size_text = f"~{size_bytes} B"
        except Exception:
            size_text = "--"
        info_card = tk.Frame(self._history, bg=_SAO_PANEL_HEADER_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        info_card.pack(fill='x', pady=(0, 8))
        for info_label, info_value, info_fg in (
            ('Encounter', title, _SAO_PANEL_GOLD),
            ('事件数', f"{event_count:,}", _SAO_PANEL_VALUE_FG),
            ('大小估计', size_text, _SAO_PANEL_VALUE_FG),
        ):
            row_frame = tk.Frame(info_card, bg=_SAO_PANEL_HEADER_BG)
            row_frame.pack(fill='x', padx=10, pady=4)
            tk.Label(row_frame, text=info_label, bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(9), anchor='w').pack(side='left')
            tk.Label(row_frame, text=info_value, bg=_SAO_PANEL_HEADER_BG, fg=info_fg,
                     font=get_cjk_font(9, True), anchor='e').pack(side='right')
        if not history:
            return
        for idx, item in enumerate(history[:12]):
            if not isinstance(item, Mapping):
                continue
            history_index = _finite_int(
                item.get('_history_index') if item.get('_history_index') is not None else idx,
                idx,
                lo=0,
            )
            dmg = _finite_int(item.get('total_damage'), 0)
            dps = _finite_int(item.get('total_dps'), 0)
            box = tk.Frame(self._history, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            box.pack(fill='x', pady=4)
            tk.Label(
                box,
                text=f"{item.get('completed_local_time') or item.get('report_reason') or 'Encounter'}\nDMG {dmg:,} · DPS {dps:,}",
                bg=_SAO_PANEL_BODY_BG,
                fg=_SAO_PANEL_LABEL_FG,
                anchor='w',
                justify='left',
                font=get_cjk_font(9),
                padx=8,
                pady=7,
            ).pack(fill='x')
            actions = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
            actions.pack(fill='x', padx=6, pady=(0, 6))
            for label, command, kind in (
                ('载入 Load', lambda i=history_index: self.load_history(i), 'cyan'),
                ('删除 Delete', lambda i=history_index: self.delete_history(i), 'danger'),
            ):
                components.action_button(actions, label, command, kind=kind).pack(
                    side='left', fill='x', expand=True, padx=(0, 4))

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
            font=get_cjk_font(10),
            pady=28,
        ).pack(fill='x')
