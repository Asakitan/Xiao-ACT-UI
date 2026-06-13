# -*- coding: utf-8 -*-
"""Entity-mode memory-scan explorer (Mem Scope) panel.

Browses every memory-scan-readable resource through the read-only ``MemAccess``
facade (``act_mem_*`` runtime actions), shows live self / entities / damage, and
runs an asynchronous manual value search where each hit carries a decoded hint.
The WebView twin is ``web/mem_scope.html`` — both consume the same JSON contract
(``act_mem_scope_status`` + ``act_mem_search*``) so they stay 1:1.
"""

from __future__ import annotations

import json
import math
import time
import tkinter as tk
from typing import Any, Dict, List, Mapping, Optional

from act_platform.runtime import (
    act_mem_scope_status,
    act_mem_search,
    act_mem_search_status,
    act_mem_narrow,
    act_mem_search_cancel,
    act_mem_attr_map,
    act_render_apply_hooks,
)
from gui_modules.sao_panel_components import (
    keep_canvas_scroll,
    sao_entry,
    sao_option_menu,
    sao_scrollbar,
    SP_SM,
    SP_MD,
    action_button,
    empty_state,
    section_card,
    status_badge,
)
from utils.sao_sound import get_sao_font, get_cjk_font
from gui_modules.sao_panel_ui import (
    _SAO_PANEL_BG,
    _SAO_PANEL_BODY_BG,
    _SAO_PANEL_GOLD,
    _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
    _theme_color,
)

_DTYPES = ("i32", "u32", "i64", "u64", "f32", "utf16")
_POLL_MS = 500


def _finite_float(value: Any, default: float = 0.0, *, lo: Optional[float] = None,
                  hi: Optional[float] = None) -> float:
    try:
        number = float(default if value is None or value == '' else value)
    except Exception:
        number = float(default or 0.0)
    if not math.isfinite(number):
        number = float(default or 0.0)
    if lo is not None:
        number = max(float(lo), number)
    if hi is not None:
        number = min(float(hi), number)
    return number


def _finite_int(value: Any, default: int = 0, *, lo: Optional[int] = None,
                hi: Optional[int] = None) -> int:
    number = _finite_float(value, float(default or 0), lo=lo, hi=hi)
    return int(number)


class MemScopePanel:
    """内存浏览器（CE-lite，全只读）：暴露 mem_scan 全部可读资源 + 手动按值搜索。"""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._rows: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="MEM SCOPE: --")
        self._status_var = tk.StringVar(value="Ready")
        self._query_var = tk.StringVar(value="")
        self._dtype_var = tk.StringVar(value="i32")
        self._job_id = ""
        self._poll_after: Optional[str] = None
        self._last_status: Dict[str, Any] = {}
        self._last_sig = ""

    # ── window lifecycle (mirrors ActAggregatePanel) ──────────────────────────
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
        self._stop_poll()
        if self._win is None:
            return
        try:
            self._win.withdraw()
        except Exception:
            pass

    def destroy(self) -> None:
        self._stop_poll()
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

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    # ── data ──────────────────────────────────────────────────────────────────
    def refresh(self) -> Dict[str, Any]:
        try:
            status = act_mem_scope_status(self.owner, query=self._query_var.get(),
                                          dtype=self._dtype_var.get(), job_id=self._job_id)
        except Exception as exc:
            status = {"ok": False, "status": {"active": False, "hint": str(exc)},
                      "catalog": {"categories": []}, "errors": [str(exc)]}
        try:
            hooked = act_render_apply_hooks(self.owner, 'mem_scope', status)
            if hooked.get('ok') and isinstance(hooked.get('payload'), dict):
                status = hooked['payload']
        except Exception:
            pass
        self._last_status = status
        self._render_status(status)
        return status

    def do_search(self) -> Dict[str, Any]:
        value = self._query_var.get().strip()
        if not value:
            self._status_var.set("请输入要搜索的值")
            return {"ok": False}
        res = act_mem_search(self.owner, value=value, dtype=self._dtype_var.get())
        if res.get("ok"):
            self._job_id = str(res.get("job_id") or "")
            self._status_var.set(f"搜索中… ({self._dtype_var.get()} = {value})")
            self._start_poll()
        else:
            self._status_var.set(f"搜索失败: {res.get('hint') or res.get('reason')}")
        return res

    def do_narrow(self) -> Dict[str, Any]:
        if not self._job_id:
            return {"ok": False}
        value = self._query_var.get().strip()
        res = act_mem_narrow(self.owner, job_id=self._job_id, value=value)
        if res.get("ok"):
            self._status_var.set(f"收敛中… (= {value})")
            self._start_poll()
        else:
            self._status_var.set(f"收敛失败: {res.get('hint') or res.get('reason')}")
        return res

    def do_clear(self) -> None:
        if self._job_id:
            try:
                act_mem_search_cancel(self.owner, job_id=self._job_id)
            except Exception:
                pass
        self._job_id = ""
        self._stop_poll()
        self._status_var.set("已清除搜索")
        self.refresh()

    def copy_json(self) -> Dict[str, Any]:
        text = json.dumps(self._last_status or {}, ensure_ascii=False, indent=2, default=str)
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self._status_var.set("Mem Scope JSON copied")
            return {"ok": True}
        except Exception as exc:
            self._status_var.set(str(exc))
            return {"ok": False, "message": str(exc)}

    def _start_poll(self) -> None:
        self._stop_poll()
        self._poll_after = self.root.after(_POLL_MS, self._poll)

    def _stop_poll(self) -> None:
        if self._poll_after is not None:
            try:
                self.root.after_cancel(self._poll_after)
            except Exception:
                pass
            self._poll_after = None

    def _poll(self) -> None:
        self._poll_after = None
        if not self._job_id or not self.is_visible():
            return
        try:
            js = act_mem_search_status(self.owner, job_id=self._job_id)
        except Exception:
            js = {}
        self.refresh()
        if not js.get("done"):
            self._poll_after = self.root.after(_POLL_MS, self._poll)

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO Mem Scope')
        win.geometry('980x720+200+90')
        win.minsize(760, 480)
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
        header = _sao_panel_header(win, 'MEM SCOPE · 内存浏览器', on_close=self.hide, flat=True)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=14, pady=(12, 8))
        _sao_pill(toolbar, 'MEM').pack(side='left')
        action_button(toolbar, '关闭 Close', self.hide).pack(side='right', padx=(6, 0))
        action_button(toolbar, '复制 Copy', self.copy_json, kind='cyan').pack(side='right', padx=(6, 0))
        action_button(toolbar, '刷新 Refresh', self.refresh, kind='gold').pack(side='right', padx=(6, 0))
        # 按钮先 pack — 窄窗下 summary 不挤按钮
        tk.Label(toolbar, textvariable=self._summary_var, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=get_cjk_font(10, True)).pack(side='left', padx=(12, 0))

        control = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        control.pack(fill='x', padx=14, pady=(0, 8))
        tk.Label(control, text='类型', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                 font=get_cjk_font(9)).pack(side='left')
        sao_option_menu(control, self._dtype_var, *_DTYPES).pack(side='left', padx=(6, 8))
        tk.Label(control, text='搜索值', bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                 font=get_cjk_font(9)).pack(side='left')
        entry = sao_entry(control, textvariable=self._query_var, width=22)
        entry.pack(side='left', padx=(6, 8))
        entry.bind('<Return>', lambda _e: self.do_search())
        action_button(control, '搜索 Search', self.do_search, kind='gold').pack(side='left', padx=(0, 4))
        action_button(control, '收敛 Narrow', self.do_narrow, kind='cyan').pack(side='left', padx=(0, 4))
        action_button(control, '清除 Clear', self.do_clear).pack(side='left')

        tk.Label(body, textvariable=self._status_var, anchor='w', bg=_SAO_PANEL_BODY_BG,
                 fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(9)).pack(fill='x', padx=14, pady=(0, 6))

        outer = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        outer.pack(fill='both', expand=True, padx=14, pady=(0, 14))
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
        self._last_sig = ""

    # ── render ─────────────────────────────────────────────────────────────────
    def _render_status(self, status: Mapping[str, Any]) -> None:
        st = status.get('status') if isinstance(status.get('status'), Mapping) else {}
        active = bool(st.get('active'))
        self._summary_var.set(
            f"{'HYBRID' if active else 'TCP-ONLY'} · {st.get('process') or '-'} · GA {st.get('module_base') or '-'}"
        )
        if self._rows is None:
            return
        sig = self._signature(status)
        if sig == self._last_sig:
            return
        self._last_sig = sig
        keep_canvas_scroll(getattr(self, '_canvas', None), self._rows)
        for child in list(self._rows.winfo_children()):
            child.destroy()

        badges = tk.Frame(self._rows, bg=_SAO_PANEL_BODY_BG)
        badges.pack(fill='x', padx=4, pady=(0, 8))
        status_badge(badges, 'HYBRID' if active else 'TCP-ONLY',
                     kind='ok' if active else 'warn').pack(side='left', padx=(0, SP_SM))
        if st.get('provider_mode'):
            status_badge(badges, f"reader={st.get('provider_mode')}", kind='cyan').pack(side='left', padx=(0, SP_SM))
        if st.get('armed'):
            status_badge(badges, 'armed', kind='gold').pack(side='left')

        if not active:
            empty_state(self._rows, '内存扫描未启用',
                        str(st.get('hint') or '切换数据源到 hybrid 模式以启用内存浏览器。')).pack(
                fill='x', padx=4, pady=10)
            self._render_catalog(status, hint_only=True)
            return

        self._render_catalog(status)
        self._render_self(status)
        self._render_entities(status)
        self._render_damage(status)
        self._render_search(status)

    def _render_catalog(self, status: Mapping[str, Any], *, hint_only: bool = False) -> None:
        cat = status.get('catalog') if isinstance(status.get('catalog'), Mapping) else {}
        cats = [c for c in (cat.get('categories') or []) if isinstance(c, Mapping)]
        if not cats:
            return
        box = section_card(self._rows, '资源目录 Catalog', badge=str(len(cats)), accent='gold')
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        inner = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=SP_SM, pady=SP_SM)
        for c in cats:
            row = tk.Frame(inner, bg=_SAO_PANEL_BODY_BG)
            row.pack(fill='x', pady=1)
            avail = bool(c.get('available'))
            status_badge(row, '可用' if avail else '未就绪', kind='ok' if avail else 'warn').pack(side='left', padx=(0, 6))
            tk.Label(row, text=str(c.get('name') or c.get('id') or ''), bg=_SAO_PANEL_BODY_BG,
                     fg=_SAO_PANEL_GOLD, font=get_cjk_font(9, True), width=18, anchor='w').pack(side='left')
            tk.Label(row, text=str(c.get('hint') or ''), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(8), anchor='w', justify='left', wraplength=680).pack(
                side='left', fill='x', expand=True)

    def _render_self(self, status: Mapping[str, Any]) -> None:
        self_ = status.get('self') if isinstance(status.get('self'), Mapping) else {}
        if not self_ or not self_.get('ok'):
            return
        box = section_card(self._rows, '自身状态 Self', accent='cyan')
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        inner = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=SP_SM, pady=SP_SM)
        skip = {'ok', 'reason', 'hint', 'resources'}
        for k, v in self_.items():
            if k in skip:
                continue
            self._kv(inner, k, v)

    def _render_entities(self, status: Mapping[str, Any]) -> None:
        ents = status.get('entities') if isinstance(status.get('entities'), Mapping) else {}
        rows = [e for e in (ents.get('entities') or []) if isinstance(e, Mapping)]
        box = section_card(self._rows, '实体 Entities', badge=str(len(rows)), accent='danger')
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        inner = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=SP_SM, pady=SP_SM)
        if not rows:
            empty_state(inner, '暂无实体', '当前没有可见战斗实体（开怪后出现）。').pack(fill='x')
            return
        cols = (('kind', '类型', 7), ('name', '名字', 14), ('cur_hp', 'HP', 10),
                ('max_hp', 'MaxHP', 10), ('hp_pct', '%', 6), ('base_id', 'BaseId', 8), ('obj', '地址', 16))
        self._table(inner, cols, rows[:40])

    def _render_damage(self, status: Mapping[str, Any]) -> None:
        dmg = status.get('damage') if isinstance(status.get('damage'), Mapping) else {}
        totals = dmg.get('totals') if isinstance(dmg.get('totals'), Mapping) else {}
        box = section_card(self._rows, '伤害总表 Damage', badge=str(len(totals)), accent='gold')
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        inner = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=SP_SM, pady=SP_SM)
        if not totals:
            empty_state(inner, '暂无伤害数据', '开始战斗后游戏会聚合每玩家伤害。').pack(fill='x')
            return
        items = sorted(totals.items(), key=lambda kv: -_to_num(kv[1]))[:20]
        rows = [{'uid': k, 'total': v} for k, v in items]
        self._table(inner, (('uid', 'UID', 18), ('total', '总伤害', 16)), rows)

    def _render_search(self, status: Mapping[str, Any]) -> None:
        search = status.get('search') if isinstance(status.get('search'), Mapping) else {}
        results = [r for r in (search.get('results') or []) if isinstance(r, Mapping)]
        count = _finite_int(search.get('count'), 0, lo=0)
        badge = f"{len(results)}/{count}" if count else "0"
        box = section_card(self._rows, '搜索结果 Search', badge=badge, accent='cyan')
        box.pack(fill='x', padx=4, pady=(SP_MD, 0))
        inner = tk.Frame(box, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill='x', padx=SP_SM, pady=SP_SM)
        if not self._job_id:
            empty_state(inner, '手动搜索', '输入一个值（如当前 HP），选类型，点「搜索」。命中后变化数值再点「收敛」逐步缩小。').pack(fill='x')
            return
        state = str(search.get('state') or '')
        if search.get('error'):
            tk.Label(inner, text=f"错误: {search.get('error')}", bg=_SAO_PANEL_BODY_BG, fg=_theme_color('danger', '#ff6b82'),
                     font=get_cjk_font(9), anchor='w').pack(fill='x')
            return
        if state == 'running':
            pct = int(_finite_float(search.get('progress'), 0.0, lo=0.0, hi=1.0) * 100)
            tk.Label(inner, text=f"扫描中… {pct}%", bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(9), anchor='w').pack(fill='x')
            return
        if not results:
            empty_state(inner, '无命中', f'未找到匹配（共 {count} 个，可能为 0）。').pack(fill='x')
            return
        if search.get('truncated'):
            tk.Label(inner, text=f"显示 {len(results)} / {count} 条（点行复制地址，点「attr」按地址读属性表）",
                     bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG, font=get_cjk_font(8), anchor='w').pack(fill='x')
        for r in results:
            addr = str(r.get('addr') or '')
            line = _fmt_hint(r)
            row = tk.Frame(inner, bg=_SAO_PANEL_BODY_BG)
            row.pack(fill='x', pady=1)
            action_button(row, 'attr', lambda a=addr: self._read_attr(a)).pack(side='right', padx=(4, 0))
            action_button(row, '复制', lambda a=addr: self._copy_addr(a), kind='cyan').pack(side='right', padx=(4, 0))
            tk.Label(row, text=line, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG, font=('Consolas', 8),
                     anchor='w', justify='left', wraplength=720).pack(side='left', fill='x', expand=True)

    def _read_attr(self, addr: str) -> None:
        try:
            res = act_mem_attr_map(self.owner, ent_addr=addr)
        except Exception as exc:
            res = {"ok": False, "hint": str(exc)}
        if res.get('ok'):
            attrs = res.get('attrs') or {}
            self._status_var.set(f"{addr} attr_map: {len(attrs)} 项 -> {dict(list(attrs.items())[:6])}")
        else:
            self._status_var.set(f"{addr}: {res.get('hint') or res.get('reason')}")

    def _copy_addr(self, addr: str) -> None:
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(addr)
            self._status_var.set(f"已复制 {addr}")
        except Exception as exc:
            self._status_var.set(f"复制失败: {exc}")

    # ── small render helpers ───────────────────────────────────────────────────
    def _kv(self, parent: tk.Misc, label: Any, value: Any) -> None:
        row = tk.Frame(parent, bg=_SAO_PANEL_BODY_BG)
        row.pack(fill='x', pady=1)
        tk.Label(row, text=str(label), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                 font=get_cjk_font(9), anchor='w').pack(side='left')
        tk.Label(row, text=str(value), bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                 font=get_cjk_font(9, True), anchor='e').pack(side='right')

    def _table(self, parent: tk.Misc, cols, rows: List[Mapping[str, Any]]) -> None:
        grid = tk.Frame(parent, bg=_SAO_PANEL_BODY_BG)
        grid.pack(fill='x')
        for ci, (_key, title, width) in enumerate(cols):
            grid.grid_columnconfigure(ci, weight=(1 if ci <= 1 else 0))
            tk.Label(grid, text=title, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                     font=get_cjk_font(8, True), anchor='w').grid(row=0, column=ci, sticky='ew', padx=4, pady=(0, 2))
        for ri, row in enumerate(rows, start=1):
            for ci, (key, _title, width) in enumerate(cols):
                val = row.get(key)
                tk.Label(grid, text='' if val is None else str(val), bg=_SAO_PANEL_BODY_BG,
                         fg=_SAO_PANEL_VALUE_FG, font=('Consolas', 8), anchor='w', width=width).grid(
                    row=ri, column=ci, sticky='ew', padx=4, pady=1)

    def _signature(self, status: Mapping[str, Any]) -> str:
        st = status.get('status') if isinstance(status.get('status'), Mapping) else {}
        search = status.get('search') if isinstance(status.get('search'), Mapping) else {}
        ents = status.get('entities') if isinstance(status.get('entities'), Mapping) else {}
        dmg = status.get('damage') if isinstance(status.get('damage'), Mapping) else {}
        cat = status.get('catalog') if isinstance(status.get('catalog'), Mapping) else {}
        self_status = status.get('self') if isinstance(status.get('self'), Mapping) else {}
        entity_rows = []
        for row in [e for e in (ents.get('entities') or []) if isinstance(e, Mapping)][:40]:
            entity_rows.append({
                key: row.get(key)
                for key in ('kind', 'name', 'cur_hp', 'max_hp', 'hp_pct', 'base_id', 'obj')
            })
        totals = dmg.get('totals') if isinstance(dmg.get('totals'), Mapping) else {}
        damage_rows = [
            {'uid': key, 'total': value}
            for key, value in sorted(totals.items(), key=lambda kv: (-_to_num(kv[1]), str(kv[0])))[:20]
        ]
        search_results = []
        for row in [r for r in (search.get('results') or []) if isinstance(r, Mapping)]:
            search_results.append({
                'addr': row.get('addr'),
                'as': row.get('as'),
                'in': row.get('in'),
                'klass_hint': row.get('klass_hint'),
            })
        self_rows = {
            str(key): value
            for key, value in self_status.items()
            if key not in {'ok', 'reason', 'hint', 'resources'}
        }
        catalog_rows = [
            {
                'id': row.get('id'),
                'name': row.get('name'),
                'available': bool(row.get('available')),
                'hint': row.get('hint'),
            }
            for row in (cat.get('categories') or [])
            if isinstance(row, Mapping)
        ]
        return json.dumps({
            'status': {
                'active': bool(st.get('active')),
                'armed': bool(st.get('armed')),
                'process': st.get('process'),
                'module_base': st.get('module_base'),
                'provider_mode': st.get('provider_mode'),
                'hint': st.get('hint'),
            },
            'job_id': self._job_id,
            'catalog': catalog_rows,
            'self': self_rows,
            'entities': entity_rows,
            'damage': damage_rows,
            'search': {
                'state': search.get('state'),
                'count': search.get('count'),
                'progress': search.get('progress'),
                'error': search.get('error'),
                'truncated': bool(search.get('truncated')),
                'results': search_results,
            },
        }, ensure_ascii=False, sort_keys=True, default=str)


def _to_num(v: Any) -> float:
    try:
        return float(v)
    except Exception:
        try:
            return float(int(str(v), 0))
        except Exception:
            return 0.0


def _fmt_hint(r: Mapping[str, Any]) -> str:
    """Compose one readable line from a decode_hint dict."""
    addr = str(r.get('addr') or '')
    av = r.get('as') if isinstance(r.get('as'), Mapping) else {}
    bits = [addr]
    if 'u32' in av:
        bits.append(f"u32={av['u32']}")
    if 'i32' in av and av.get('i32') != av.get('u32'):
        bits.append(f"i32={av['i32']}")
    if 'f32' in av:
        bits.append(f"f32={av['f32']}")
    if av.get('utf16'):
        bits.append(f"'{av['utf16']}'")
    elif av.get('cstr'):
        bits.append(f"\"{av['cstr']}\"")
    if av.get('ptr'):
        bits.append(f"->{av['ptr']}")
    loc = r.get('in') if isinstance(r.get('in'), Mapping) else {}
    if loc.get('module'):
        bits.append(f"in {loc['module']}+{loc.get('offset')}")
    elif loc.get('region'):
        bits.append(f"[{loc['region']}]")
    if r.get('klass_hint'):
        bits.append(f"~{r['klass_hint']}")
    return ' · '.join(str(b) for b in bits)
