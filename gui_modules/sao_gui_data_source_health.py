# -*- coding: utf-8 -*-
"""Entity-mode ACT data-source health panel."""

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import act_data_source_diagnose, act_data_source_health
from gui_modules.sao_panel_components import keep_canvas_scroll
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


def _finite_int(value: Any, default: int = 0, *, lo: Optional[int] = None, hi: Optional[int] = None) -> int:
    try:
        num = float(default if value is None or value == '' else value)
    except Exception:
        num = float(default or 0)
    if not math.isfinite(num):
        num = float(default or 0)
    if lo is not None:
        num = max(float(lo), num)
    if hi is not None:
        num = min(float(hi), num)
    return int(num)


class DataSourceHealthPanel:
    """SAO-styled Toplevel for ACT data-source observability."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._list: Optional[tk.Frame] = None
        self._diag: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="SOURCE: --")
        self._status_var = tk.StringVar(value="Ready")
        self._last_status: Dict[str, Any] = {}
        self._last_refresh_at = 0.0
        self._last_request_key: tuple[str, ...] = ()
        self._last_sources_sig = ""
        self._last_diag_sig = ""

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
        self._list = None
        self._diag = None
        self._reset_render_cache()

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        now = time.time()
        request_key = ("health",)
        if self._last_status and request_key == self._last_request_key and now - self._last_refresh_at < 0.35:
            self._render_status(self._last_status)
            return self._last_status
        try:
            status = act_data_source_health(self.owner)
        except Exception as exc:
            status = {"ok": False, "status": "error", "sources": {"summary": {}}, "latency_ms": 0, "last_event_ms": 0, "errors": [str(exc)]}
        self._last_status = dict(status or {})
        self._last_refresh_at = now
        self._last_request_key = request_key
        self._render_status(self._last_status)
        return self._last_status

    def diagnose(self) -> Dict[str, Any]:
        try:
            status = act_data_source_diagnose(self.owner)
        except Exception as exc:
            status = {"ok": False, "status": "error", "sources": {"summary": {}}, "latency_ms": 0, "last_event_ms": 0, "errors": [str(exc)], "diagnostics": [{"level": "error", "message": str(exc)}]}
        self._last_status = dict(status or {})
        self._last_refresh_at = time.time()
        self._last_request_key = ("diagnose",)
        self._render_status(self._last_status)
        return self._last_status

    def copy_snapshot(self) -> Dict[str, Any]:
        if not self._last_status:
            self.refresh()
        text = json.dumps(self._last_status, ensure_ascii=False, indent=2)
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set('Health snapshot copied to clipboard')
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
        win.title('SAO ACT Data Source Health')
        win.geometry('820x540+210+155')
        win.minsize(660, 420)
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
        header = _sao_panel_header(win, 'ACT DATA SOURCE HEALTH', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'OBSERVABILITY').pack(side='left')
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('诊断 Diagnose', self.diagnose),
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

        # 按钮先 pack — 窄窗下 summary 不挤按钮(后包者只分剩余空间)
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(side='left', padx=(12, 0))
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
        right = tk.Frame(outer, bg=_SAO_PANEL_BODY_BG, width=260)
        right.pack(side='right', fill='y')
        right.pack_propagate(False)

        canvas = tk.Canvas(left, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = tk.Scrollbar(left, orient='vertical', command=canvas.yview)
        self._list = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._list.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._list, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas

        tk.Label(
            right,
            text='DIAGNOSTICS',
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(fill='x', pady=(2, 8))
        self._diag = tk.Frame(right, bg=_SAO_PANEL_BODY_BG)
        self._diag.pack(fill='both', expand=True)
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        sources = status.get('sources') or {}
        summary = sources.get('summary') or {}
        source_label = str(summary.get('data_source') or status.get('requested_mode') or '--').upper()
        self._summary_var.set(f'SOURCE: {source_label}')
        errors = status.get('errors') or []
        state = str(status.get('status') or ('running' if status.get('ok') else 'error')).upper()
        latency_ms = _finite_int(status.get('latency_ms'), 0, lo=0)
        last_event_ms = _finite_int(status.get('last_event_ms'), 0, lo=0)
        self._status_var.set(
            f"{state} · latency={latency_ms}ms · last_event={last_event_ms}ms · errors={len(errors)}"
        )
        if self._list is not None:
            sources_sig = self._sources_signature(sources)
            if sources_sig == self._last_sources_sig:
                self._render_diagnostics(status)
                return
            self._last_sources_sig = sources_sig
            keep_canvas_scroll(getattr(self, '_canvas', None), self._list)
            for child in list(self._list.winfo_children()):
                child.destroy()
            rendered = False
            for key in ('packet', 'memory'):
                source = sources.get(key)
                if isinstance(source, Mapping):
                    self._render_source(key, source)
                    rendered = True
            if not rendered:
                self._render_empty()
        self._render_diagnostics(status)

    def _render_empty(self) -> None:
        if self._list is None:
            return
        box = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(
            box,
            text='没有可用的数据源\n启动识别后 PacketBridge 会提供 health 快照。',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=('Segoe UI', 10),
            pady=28,
        ).pack(fill='x')

    def _render_source(self, key: str, source: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        active = bool(source.get('running') or source.get('alive') or source.get('active') or source.get('started') or source.get('is_memory_active'))
        error = str(source.get('error_msg') or source.get('last_error') or '').strip()
        border = _SAO_PANEL_GOLD if key == 'memory' and active else (_SAO_PANEL_BORDER if active else (_SAO_PANEL_SEP if not error else '#ff6b82'))
        card = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=border)
        card.pack(fill='x', pady=6, padx=4)

        top = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        top.pack(fill='x', padx=10, pady=(8, 2))
        tk.Label(
            top,
            text=key.upper(),
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_VALUE_FG,
            anchor='w',
            font=('Segoe UI', 11, 'bold'),
        ).pack(side='left', fill='x', expand=True)
        _sao_pill(top, 'ACTIVE' if active else ('ERROR' if error else 'IDLE')).pack(side='right')

        meta = tk.Label(
            card,
            text=self._format_source(source),
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            anchor='w',
            justify='left',
            font=('Consolas', 9),
        )
        meta.pack(fill='x', padx=10, pady=(2, 4))
        watchers = source.get('watchers') or {}
        if isinstance(watchers, Mapping) and watchers:
            wline = ' · '.join(f'{name}:{mode}' for name, mode in watchers.items())
            tk.Label(
                card,
                text=wline,
                bg='#07111c',
                fg='#bfe6ff',
                anchor='w',
                justify='left',
                wraplength=490,
                font=('Consolas', 8),
                padx=8,
                pady=5,
            ).pack(fill='x', padx=10, pady=(0, 6))
        self_state = source.get('self') or {}
        if isinstance(self_state, Mapping) and self_state.get('uid'):
            tk.Label(
                card,
                text=f"self uid={self_state.get('uid')} hp={self_state.get('hp')}/{self_state.get('max_hp')} name={self_state.get('name', '')}",
                bg=_SAO_PANEL_BODY_BG,
                fg=_SAO_PANEL_GOLD,
                anchor='w',
                justify='left',
                font=('Consolas', 8),
            ).pack(fill='x', padx=10, pady=(0, 8))

    def _render_diagnostics(self, status: Mapping[str, Any]) -> None:
        if self._diag is None:
            return
        items = status.get('diagnostics') or []
        if not items:
            errors = status.get('errors') or []
            items = [{'level': 'error', 'message': err} for err in errors] if errors else [{'level': 'info', 'message': 'Click Diagnose for detailed checks.'}]
        diag_sig = repr([(str(item.get('level') or ''), str(item.get('message') or '')) for item in items if isinstance(item, Mapping)])
        if diag_sig == self._last_diag_sig:
            return
        self._last_diag_sig = diag_sig
        for child in list(self._diag.winfo_children()):
            child.destroy()
        for item in items:
            level = str(item.get('level') or 'info').upper() if isinstance(item, Mapping) else 'INFO'
            message = str(item.get('message') or '') if isinstance(item, Mapping) else str(item)
            color = '#ff6b82' if level == 'ERROR' else (_SAO_PANEL_GOLD if level == 'WARN' else _SAO_PANEL_LABEL_FG)
            box = tk.Frame(self._diag, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
            box.pack(fill='x', pady=4)
            tk.Label(
                box,
                text=f'{level}\n{message}',
                bg=_SAO_PANEL_BODY_BG,
                fg=color,
                anchor='w',
                justify='left',
                wraplength=230,
                font=('Segoe UI', 9),
                padx=8,
                pady=7,
            ).pack(fill='x')

    def _reset_render_cache(self) -> None:
        self._last_sources_sig = ""
        self._last_diag_sig = ""

    @staticmethod
    def _sources_signature(sources: Mapping[str, Any]) -> str:
        compact = {}
        for key in ('packet', 'memory', 'summary'):
            source = sources.get(key) if isinstance(sources, Mapping) else None
            if not isinstance(source, Mapping):
                continue
            compact[key] = {
                'data_source': source.get('data_source'),
                'mode': source.get('mode'),
                'status': source.get('status'),
                'requested_mode': source.get('requested_mode'),
                'uptime_s': source.get('uptime_s'),
                'running': bool(source.get('running')),
                'alive': bool(source.get('alive')),
                'active': bool(source.get('active')),
                'started': bool(source.get('started')),
                'is_memory_active': bool(source.get('is_memory_active')),
                'error': source.get('error_msg') or source.get('last_error'),
                'watchers': source.get('watchers'),
                'self': source.get('self'),
                'primary': source.get('primary'),
                'hybrid': bool(source.get('hybrid')),
                'packet_active': bool(source.get('packet_active')),
                'memory_active': bool(source.get('memory_active')),
                'parser_adapter_selection': source.get('parser_adapter_selection'),
                'parser_adapter_id': source.get('parser_adapter_id'),
                'parser_adapter_requested_id': source.get('parser_adapter_requested_id'),
                'parser_adapter_mode': source.get('parser_adapter_mode'),
                'parser_adapter_fallback_reason': source.get('parser_adapter_fallback_reason'),
            }
        return json.dumps(compact, ensure_ascii=False, sort_keys=True, default=str)

    @staticmethod
    def _format_source(source: Mapping[str, Any]) -> str:
        selection = source.get('parser_adapter_selection') if isinstance(source.get('parser_adapter_selection'), Mapping) else {}
        parser_id = selection.get('selected_id') or source.get('parser_adapter_id') or '-'
        requested_id = selection.get('requested_id') or source.get('parser_adapter_requested_id') or '-'
        parser_mode = selection.get('mode') or source.get('parser_adapter_mode') or '-'
        fallback_reason = selection.get('fallback_reason') or source.get('parser_adapter_fallback_reason') or '-'
        return (
            f"data_source={source.get('data_source') or source.get('mode') or '-'}\n"
            f"status={source.get('status') or source.get('mode') or '-'} running={bool(source.get('running'))} alive={bool(source.get('alive'))}\n"
            f"requested={source.get('requested_mode') or '-'} uptime={source.get('uptime_s') or 0}s\n"
            f"parser={parser_id} requested={requested_id} mode={parser_mode}\n"
            f"parser_fallback={fallback_reason}\n"
            f"error={source.get('error_msg') or source.get('last_error') or '-'}"
        )
