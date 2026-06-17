# -*- coding: utf-8 -*-
"""Entity-mode ACT data-source health panel."""

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, Mapping, Optional

from act_platform.runtime import act_data_source_diagnose, act_data_source_health
from gui_modules.sao_panel_components import (
    SP_SM, SP_MD, SP_XL,
    _pc, _accent, _accent_text,
    action_button, keep_canvas_scroll, metric_tile, rounded_panel,
    sao_scrollbar, section_card, status_badge,
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
    _SAO_PANEL_SEP,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _make_panel_close_button,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
    _theme_color,
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
        win.geometry('960x862+210+155')
        win.minsize(660, 420)
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
        header = _sao_panel_header(win, 'PIPELINE DIAGNOSTICS', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        bg = _pc('body_bg', _SAO_PANEL_BODY_BG)

        toolbar = tk.Frame(body, bg=bg)
        toolbar.pack(fill='x', padx=SP_MD, pady=(7, 10))
        title_box = tk.Frame(toolbar, bg=bg)
        title_box.pack(side='left', anchor='n')
        tk.Label(title_box, text='PIPELINE DIAGNOSTICS', bg=bg,
                 fg=_SAO_PANEL_GOLD, font=get_sao_font(8, True), anchor='w').pack(fill='x')
        tk.Label(title_box, text='SOURCE HEALTH 数据源健康', bg=bg,
                 fg=_SAO_PANEL_VALUE_FG, font=get_sao_font(15, True), anchor='w').pack(fill='x', pady=(1, 0))
        controls = tk.Frame(toolbar, bg=bg)
        controls.pack(side='right', anchor='center')
        self._badge_frame = tk.Frame(controls, bg=bg)
        self._badge_frame.pack(side='left', padx=(0, 8))
        action_button(controls, '复制快照', self.copy_snapshot).pack(side='left', padx=(0, 6))
        action_button(controls, '刷新', self.refresh).pack(side='left', padx=(0, 6))
        _make_panel_close_button(controls, self.hide, bg=bg, flat=True).pack(side='left', padx=(6, 0))

        self._metrics_row = tk.Frame(body, bg=bg)
        self._metrics_row.pack(fill='x', padx=SP_MD, pady=(SP_SM, SP_SM))

        canvas = tk.Canvas(body, bg=bg, highlightthickness=0, bd=0)
        scroll = sao_scrollbar(body, canvas.yview)
        self._list = tk.Frame(canvas, bg=bg)
        self._list.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        _win_id = canvas.create_window((0, 0), window=self._list, anchor='nw')
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(_win_id, width=e.width))
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self._canvas = canvas
        self._diag = None
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        sources = status.get('sources') or {}
        summary = sources.get('summary') or {}
        source_label = str(summary.get('data_source') or status.get('requested_mode') or '--').upper()
        latency_ms = _finite_int(status.get('latency_ms'), 0, lo=0)
        last_event_ms = _finite_int(status.get('last_event_ms'), 0, lo=0)
        encounter = str(summary.get('encounter') or '--')

        # Legacy summary line for headless / test callers
        self._status_var.set(f'source={source_label} latency={latency_ms}ms last_event={last_event_ms}ms')

        if not hasattr(self, '_badge_frame'):
            return

        for child in list(self._badge_frame.winfo_children()):
            child.destroy()
        status_badge(self._badge_frame, source_label, kind='cyan').pack(side='left')

        for child in list(self._metrics_row.winfo_children()):
            child.destroy()
        last_sec = f'{last_event_ms / 1000:.2f}s' if last_event_ms else '--'
        tiles = [
            ('当前源', source_label, '', 'cyan'),
            ('快照延迟', f'{latency_ms}ms', '', 'gold'),
            ('最近事件', last_sec, '前' if last_event_ms else '', 'gold'),
            ('战斗', encounter, '', 'gold'),
        ]
        for label, value, sub, accent in tiles:
            metric_tile(self._metrics_row, label, value, sub=sub, accent=accent).pack(
                side='left', fill='x', expand=True, padx=(0, SP_SM))

        if self._list is not None:
            sig = self._sources_signature(sources) + repr(status.get('diagnostics'))
            if sig == self._last_sources_sig:
                return
            self._last_sources_sig = sig
            keep_canvas_scroll(getattr(self, '_canvas', None), self._list)
            for child in list(self._list.winfo_children()):
                child.destroy()
            self._render_sources_section(sources)
            self._render_diagnostics_section(status)
            self._render_snapshot_section(status)

    _SOURCE_NAMES = {'packet': 'TCP Capture', 'memory': 'Memory Bridge', 'vision': 'Vision (STA)'}
    _SOURCE_STATUS = {True: ('ONLINE', 'ok'), False: ('OFFLINE', 'danger')}

    def _render_sources_section(self, sources: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
        source_keys = [k for k in ('packet', 'memory', 'vision') if isinstance(sources.get(k), Mapping)]
        if not source_keys:
            box = section_card(self._list, '数据源 Sources', badge='0', accent='gold')
            box.pack(fill='x', padx=SP_MD, pady=SP_SM)
            tk.Label(box, text='没有可用的数据源', bg=bg, fg=_pc('label_fg', _SAO_PANEL_LABEL_FG), font=get_cjk_font(10), pady=20).pack(fill='x')
            return
        box = section_card(self._list, '数据源 Sources', badge=str(len(source_keys)), accent='gold')
        box.pack(fill='x', padx=SP_MD, pady=SP_SM)
        for key in source_keys:
            source = sources[key]
            active = bool(source.get('running') or source.get('alive') or source.get('active'))
            error = str(source.get('error_msg') or source.get('last_error') or '').strip()
            degraded = bool(error) or (key == 'vision' and _finite_int(source.get('missed'), 0, lo=0) > 0)
            if degraded:
                badge_text, badge_kind = 'DEGRADED', 'gold'
            else:
                badge_text, badge_kind = self._SOURCE_STATUS.get(active, ('OFFLINE', 'danger'))
            name = self._SOURCE_NAMES.get(key, key.upper())
            subtitle = self._source_subtitle(key, source)
            card_frame = tk.Frame(box, bg=bg, highlightthickness=1, highlightbackground=_pc('border', _SAO_PANEL_BORDER))
            card_frame.pack(fill='x', pady=SP_SM, padx=SP_SM)
            top = tk.Frame(card_frame, bg=bg)
            top.pack(fill='x', padx=SP_MD, pady=(SP_SM, 2))
            tk.Label(top, text=name, bg=bg, fg=_pc('value_fg', _SAO_PANEL_VALUE_FG), font=get_cjk_font(10, True), anchor='w').pack(side='left', fill='x', expand=True)
            status_badge(top, badge_text, kind=badge_kind).pack(side='right')
            if subtitle:
                tk.Label(card_frame, text=subtitle, bg=bg, fg=_pc('label_fg', _SAO_PANEL_LABEL_FG), font=get_cjk_font(8), anchor='w').pack(fill='x', padx=SP_MD, pady=(0, SP_SM))

    def _render_diagnostics_section(self, status: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
        items = status.get('diagnostics') or []
        if not items:
            errors = status.get('errors') or []
            items = [{'level': 'error', 'message': err} for err in errors] if errors else []
        box = section_card(self._list, '诊断 Diagnostics', badge=str(len(items)), accent='cyan')
        box.pack(fill='x', padx=SP_MD, pady=SP_SM)
        for item in items:
            level = str(item.get('level') or 'info').upper() if isinstance(item, Mapping) else 'INFO'
            message = str(item.get('message') or '') if isinstance(item, Mapping) else str(item)
            kind = 'danger' if level == 'ERROR' else ('gold' if level == 'WARN' else 'ok')
            badge_text = level if level in ('WARN', 'ERROR') else 'OK'
            row = tk.Frame(box, bg=bg)
            row.pack(fill='x', padx=SP_SM, pady=3)
            status_badge(row, badge_text, kind=kind).pack(side='left', padx=(0, SP_SM))
            tk.Label(row, text=message, bg=bg, fg=_pc('label_fg', _SAO_PANEL_LABEL_FG), font=get_cjk_font(9), anchor='w', wraplength=500).pack(side='left', fill='x', expand=True)

    def _render_snapshot_section(self, status: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        bg = _pc('card_bg', _SAO_PANEL_BODY_BG)
        box = section_card(self._list, '快照 Snapshot', accent='cyan')
        box.pack(fill='x', padx=SP_MD, pady=SP_SM)
        snapshot_text = json.dumps(self._condensed_snapshot(status), ensure_ascii=False, indent=2, default=str)
        tk.Label(box, text=snapshot_text, bg=bg, fg=_pc('label_fg', _SAO_PANEL_LABEL_FG),
                 font=('Consolas', 8), anchor='nw', justify='left', wraplength=700).pack(fill='x', padx=SP_SM, pady=SP_SM)

    @staticmethod
    def _condensed_snapshot(status: Mapping[str, Any]) -> dict:
        """Build a compact summary matching the webref snapshot format."""
        sources = status.get('sources') or {}
        summary = sources.get('summary') or {}
        packet = sources.get('packet') or {}
        memory = sources.get('memory') or {}
        vision = sources.get('vision') or {}
        out: dict[str, Any] = {
            'data_source': summary.get('data_source') or status.get('requested_mode') or '--',
        }
        if packet:
            tcp: dict[str, Any] = {}
            if packet.get('hz') is not None:
                try:
                    tcp['hz'] = round(float(packet['hz']), 1)
                except (ValueError, TypeError):
                    pass
            tcp['drops'] = _finite_int(packet.get('dropped'), 0, lo=0)
            out['tcp'] = tcp
        if memory:
            mem: dict[str, Any] = {}
            mode = str(memory.get('mode') or '').strip()
            if mode:
                mem['mode'] = mode
            if memory.get('gated') is not None:
                mem['gated'] = bool(memory.get('gated'))
            out['memory'] = mem
        if vision:
            vis: dict[str, Any] = {}
            vmode = str(vision.get('mode') or '').strip()
            if vmode:
                vis['mode'] = vmode
            if vision.get('missed') is not None:
                vis['missed'] = _finite_int(vision.get('missed'), 0, lo=0)
            out['vision'] = vis
        out['latency_ms'] = _finite_int(status.get('latency_ms'), 0, lo=0)
        encounter = str(summary.get('encounter') or '').strip()
        if encounter:
            out['encounter'] = encounter
        events = status.get('events_emitted')
        if events is not None:
            out['events_emitted'] = _finite_int(events, 0, lo=0)
        return out

    def _reset_render_cache(self) -> None:
        self._last_sources_sig = ""
        self._last_diag_sig = ""

    @staticmethod
    def _sources_signature(sources: Mapping[str, Any]) -> str:
        compact = {}
        for key in ('packet', 'memory', 'vision', 'summary'):
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
    def _source_subtitle(key: str, source: Mapping[str, Any]) -> str:
        parts: list[str] = []
        if key == 'packet':
            # Prefer driver/backend label (e.g. "Npcap"); fall back to mode
            driver = str(source.get('driver') or source.get('backend') or '').strip()
            mode = str(source.get('mode') or '').strip()
            if driver:
                parts.append(driver)
            elif mode:
                parts.append(mode.capitalize())
            # Prefer Hz rate over raw packet count
            hz = source.get('hz')
            if hz is not None:
                try:
                    hz_val = float(hz)
                    parts.append(f'{hz_val:g} Hz')
                except (ValueError, TypeError):
                    pass
            dropped = _finite_int(source.get('dropped'), 0, lo=0)
            parts.append(f'{dropped} 丢包')
        elif key == 'memory':
            # Show data_source / requested_mode (e.g. "hybrid") not raw mode
            ds = str(source.get('data_source') or source.get('requested_mode') or '').strip()
            parts.append('IL2CPP')
            if ds:
                parts.append(ds)
            if source.get('gated') or source.get('active'):
                parts.append('已授权 · 只读')
        elif key == 'vision':
            mode = str(source.get('mode') or '').strip()
            missed = source.get('missed')
            if mode:
                parts.append('识图推断')
            if missed is not None:
                parts.append(f'{_finite_int(missed, 0, lo=0)} 帧未命中')
            elif mode:
                parts.append(mode)
        return ' · '.join(parts) if parts else ''
