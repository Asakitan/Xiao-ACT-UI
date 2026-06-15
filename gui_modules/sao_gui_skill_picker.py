# -*- coding: utf-8 -*-
"""
sao_gui_skill_picker — 自定义技能 CD 监控选择器 (Tk).

弹出面板, 列出所有可用技能 CD, 允许用户:
  - 搜索/筛选技能
  - 添加到自定义监控槽位 (10-14)
  - 设置父槽位归属 (天赋分组)
  - 开关 TTS / 视觉提醒
  - 管理已配置的监控项

WebView 对等版: web/skill_picker.html
"""
from __future__ import annotations

import tkinter as tk
from typing import Any, Dict, List, Optional

from gui_modules.sao_panel_ui import (
    _sao_panel_header, _sao_panel_body,
    _SAO_PANEL_BG, _SAO_PANEL_BODY_BG, _SAO_PANEL_HEADER_BG,
    _SAO_PANEL_ACCENT, _SAO_PANEL_GOLD, _SAO_PANEL_SEP,
    _SAO_PANEL_FG, _SAO_PANEL_MUTED, _SAO_PANEL_BORDER,
    _theme_color,
)
from gui_modules.sao_panel_components import (
    sao_entry, sao_option_menu, sao_scrollbar,
    action_button, status_badge, rounded_panel, attach_tooltip,
    SP_SM, SP_MD, SP_LG,
)
try:
    from gui_modules.sao_web_panel_common import panel_font
except Exception:
    def panel_font(size, bold=False, family='Segoe UI'):
        import tkinter.font as tkfont
        return tkfont.Font(family=family, size=size,
                           weight='bold' if bold else 'normal')

from engines.skill_cd_monitor import (
    validate_monitors, next_available_slot, MAX_CUSTOM_SLOTS,
    CUSTOM_SLOT_MIN, CUSTOM_SLOT_MAX, _resolve_skill_name,
)


def _pc(key: str, fallback: str = '') -> str:
    try:
        return _theme_color(key, fallback) or fallback
    except Exception:
        return fallback


class SkillPickerPanel:
    """Tk 技能选择器面板 — 管理自定义 CD 监控."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._query_var = tk.StringVar(value='')
        self._filter_after_id: Optional[str] = None
        self._available_frame: Optional[tk.Frame] = None
        self._configured_frame: Optional[tk.Frame] = None
        self._canvas: Optional[tk.Canvas] = None
        self._configured_canvas: Optional[tk.Canvas] = None

    def show(self):
        if self._win and self._win.winfo_exists():
            self._win.lift()
            self._win.focus_force()
            self.refresh()
            return
        self._build()
        self.refresh()

    def hide(self):
        if self._win:
            try:
                self._win.destroy()
            except Exception:
                pass
            self._win = None

    def is_visible(self) -> bool:
        return self._win is not None and self._win.winfo_exists()

    def _build(self):
        win = tk.Toplevel(self.root)
        win.title('自定义技能 CD 监控')
        win.geometry('720x680+240+120')
        win.minsize(600, 480)
        win.configure(bg=_SAO_PANEL_BG)
        win.protocol('WM_DELETE_WINDOW', self.hide)
        self._win = win

        header = _sao_panel_header(win, '自定义技能 CD 监控',
                                   on_close=self.hide, flat=True)
        header.pack(fill='x')

        body = _sao_panel_body(win, flat=True)
        body.pack(fill='both', expand=True, padx=0, pady=0)

        # ── 已配置的监控项 (顶部区域) ──
        configured_section = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        configured_section.pack(fill='x', padx=SP_MD, pady=(SP_MD, SP_SM))

        tk.Label(configured_section, text='已监控的技能',
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=panel_font(11, bold=True)).pack(anchor='w')

        conf_scroll_wrap = tk.Frame(configured_section, bg=_SAO_PANEL_BODY_BG)
        conf_scroll_wrap.pack(fill='x', pady=(SP_SM, 0))
        conf_canvas = tk.Canvas(conf_scroll_wrap, bg=_SAO_PANEL_BODY_BG,
                                highlightthickness=0, bd=0, height=160)
        conf_scroll = sao_scrollbar(conf_scroll_wrap, conf_canvas.yview)
        conf_canvas.configure(yscrollcommand=conf_scroll.set)
        conf_scroll.pack(side='right', fill='y')
        conf_canvas.pack(side='left', fill='both', expand=True)
        conf_inner = tk.Frame(conf_canvas, bg=_SAO_PANEL_BODY_BG)
        conf_wid = conf_canvas.create_window((0, 0), window=conf_inner, anchor='nw')
        conf_canvas.bind('<Configure>',
                         lambda e: conf_canvas.itemconfigure(conf_wid, width=e.width))
        conf_inner.bind('<Configure>',
                        lambda e: conf_canvas.configure(scrollregion=conf_canvas.bbox('all')))
        self._configured_frame = conf_inner
        self._configured_canvas = conf_canvas

        # ── 分隔线 ──
        tk.Frame(body, bg=_SAO_PANEL_SEP, height=1).pack(fill='x', padx=SP_MD, pady=SP_SM)

        # ── 搜索栏 ──
        search_bar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        search_bar.pack(fill='x', padx=SP_MD, pady=(0, SP_SM))

        tk.Label(search_bar, text='搜索技能',
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                 font=panel_font(11, bold=True)).pack(side='left')

        query_entry = sao_entry(search_bar, textvariable=self._query_var, width=24)
        query_entry.pack(side='left', padx=(SP_MD, SP_SM))
        query_entry.bind('<KeyRelease>', lambda _e: self._schedule_filter())
        query_entry.bind('<Return>', lambda _e: self.refresh())

        action_button(search_bar, '刷新列表', self.refresh, kind='gold').pack(
            side='left', padx=(SP_SM, 0))

        slot_info = f'已用 {{0}}/{MAX_CUSTOM_SLOTS} 槽'
        self._slot_count_var = tk.StringVar(value=slot_info.format(0))
        tk.Label(search_bar, textvariable=self._slot_count_var,
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_MUTED,
                 font=panel_font(9)).pack(side='right')

        # ── 可用技能列表 (主区域) ──
        tk.Label(body, text='可用技能 (非装备槽)',
                 bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_ACCENT,
                 font=panel_font(10, bold=True)).pack(anchor='w', padx=SP_MD)

        main_wrap = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        main_wrap.pack(fill='both', expand=True, padx=SP_MD, pady=(SP_SM, SP_MD))

        canvas = tk.Canvas(main_wrap, bg=_SAO_PANEL_BODY_BG,
                           highlightthickness=0, bd=0)
        scroll = sao_scrollbar(main_wrap, canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)

        avail_inner = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        wid = canvas.create_window((0, 0), window=avail_inner, anchor='nw')
        canvas.bind('<Configure>',
                    lambda e: canvas.itemconfigure(wid, width=e.width))
        avail_inner.bind('<Configure>',
                         lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
        self._available_frame = avail_inner
        self._canvas = canvas

    def _schedule_filter(self):
        if self._filter_after_id is not None:
            try:
                self._win.after_cancel(self._filter_after_id)
            except Exception:
                pass
        try:
            self._filter_after_id = self._win.after(300, self.refresh)
        except Exception:
            pass

    def refresh(self):
        if not self._win or not self._win.winfo_exists():
            return
        self._render_configured()
        self._render_available()

    def _render_configured(self):
        frame = self._configured_frame
        if not frame:
            return
        for child in list(frame.winfo_children()):
            child.destroy()

        monitors = self.owner._get_custom_monitors()
        self._slot_count_var.set(f'已用 {len(monitors)}/{MAX_CUSTOM_SLOTS} 槽')

        if not monitors:
            tk.Label(frame, text='尚未配置自定义技能监控',
                     bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_MUTED,
                     font=panel_font(9)).pack(anchor='w', pady=SP_SM)
            return

        # 表头
        hdr = tk.Frame(frame, bg=_SAO_PANEL_BODY_BG)
        hdr.pack(fill='x', pady=(0, 2))
        for col, (text, w) in enumerate([
            ('槽位', 4), ('技能名', 14), ('技能ID', 8),
            ('归属槽', 5), ('TTS', 4), ('视觉', 4), ('操作', 6),
        ]):
            tk.Label(hdr, text=text, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_MUTED,
                     font=panel_font(8), width=w, anchor='w').pack(side='left', padx=1)

        for idx, mon in enumerate(monitors):
            row_bg = _SAO_PANEL_BODY_BG if idx % 2 == 0 else _pc('stripe', '#1a2636')
            row = tk.Frame(frame, bg=row_bg)
            row.pack(fill='x', pady=1)

            tk.Label(row, text=str(mon['slot']), bg=row_bg, fg=_SAO_PANEL_FG,
                     font=panel_font(9), width=4, anchor='w').pack(side='left', padx=1)
            tk.Label(row, text=mon['name'], bg=row_bg, fg=_SAO_PANEL_ACCENT,
                     font=panel_font(9, bold=True), width=14, anchor='w').pack(side='left', padx=1)
            tk.Label(row, text=str(mon['skill_level_id']), bg=row_bg, fg=_SAO_PANEL_MUTED,
                     font=panel_font(8), width=8, anchor='w').pack(side='left', padx=1)

            parent_text = f'Slot {mon["parent_slot"]}' if mon['parent_slot'] > 0 else '独立'
            parent_lbl = tk.Label(row, text=parent_text, bg=row_bg,
                                  fg=_SAO_PANEL_GOLD if mon['parent_slot'] > 0 else _SAO_PANEL_MUTED,
                                  font=panel_font(8), width=5, anchor='w')
            parent_lbl.pack(side='left', padx=1)

            tts_text = '✓' if mon['tts_enabled'] else '✗'
            tts_fg = _pc('ok', '#3fae5a') if mon['tts_enabled'] else _pc('danger', '#ff707a')
            tts_btn = tk.Label(row, text=tts_text, bg=row_bg, fg=tts_fg,
                               font=panel_font(9), width=4, anchor='center', cursor='hand2')
            tts_btn.pack(side='left', padx=1)
            tts_btn.bind('<Button-1>', lambda _e, s=mon['slot'], cur=mon['tts_enabled']:
                         self._toggle_field(s, 'tts_enabled', not cur))

            vis_text = '✓' if mon['visual_enabled'] else '✗'
            vis_fg = _pc('ok', '#3fae5a') if mon['visual_enabled'] else _pc('danger', '#ff707a')
            vis_btn = tk.Label(row, text=vis_text, bg=row_bg, fg=vis_fg,
                               font=panel_font(9), width=4, anchor='center', cursor='hand2')
            vis_btn.pack(side='left', padx=1)
            vis_btn.bind('<Button-1>', lambda _e, s=mon['slot'], cur=mon['visual_enabled']:
                         self._toggle_field(s, 'visual_enabled', not cur))

            del_btn = tk.Label(row, text='✕ 移除', bg=row_bg,
                               fg=_pc('danger', '#ff707a'),
                               font=panel_font(8), cursor='hand2')
            del_btn.pack(side='left', padx=(4, 0))
            del_btn.bind('<Button-1>', lambda _e, s=mon['slot']: self._remove(s))

            # 父槽位点击编辑
            parent_lbl.configure(cursor='hand2')
            parent_lbl.bind('<Button-1>', lambda _e, s=mon['slot'], cur=mon['parent_slot']:
                            self._cycle_parent(s, cur))
            attach_tooltip(parent_lbl, '点击切换归属槽位 (0=独立, 1-9=归属对应技能)')

    def _render_available(self):
        frame = self._available_frame
        if not frame:
            return
        for child in list(frame.winfo_children()):
            child.destroy()

        query = self._query_var.get().strip().lower()
        available = self.owner._get_available_skills_for_picker()
        monitors = self.owner._get_custom_monitors()
        monitored_slids = {m['skill_level_id'] for m in monitors}

        if query:
            available = [s for s in available
                         if query in s['name'].lower()
                         or query in str(s['skill_id'])
                         or query in str(s['skill_level_id'])]

        if not available:
            msg = '无匹配技能' if query else '暂无可用技能 (需要在 hybrid 模式下游戏中获取)'
            tk.Label(frame, text=msg,
                     bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_MUTED,
                     font=panel_font(9)).pack(anchor='w', pady=SP_LG)
            return

        # 表头
        hdr = tk.Frame(frame, bg=_SAO_PANEL_BODY_BG)
        hdr.pack(fill='x', pady=(0, 2))
        for text, w in [('技能名', 16), ('ID', 8), ('LevelID', 8),
                        ('CD状态', 10), ('操作', 8)]:
            tk.Label(hdr, text=text, bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_MUTED,
                     font=panel_font(8), width=w, anchor='w').pack(side='left', padx=1)

        for idx, skill in enumerate(available[:100]):
            slid = skill['skill_level_id']
            already = slid in monitored_slids
            row_bg = _SAO_PANEL_BODY_BG if idx % 2 == 0 else _pc('stripe', '#1a2636')
            row = tk.Frame(frame, bg=row_bg)
            row.pack(fill='x', pady=1)

            name_fg = _SAO_PANEL_MUTED if already else _SAO_PANEL_FG
            tk.Label(row, text=skill['name'], bg=row_bg, fg=name_fg,
                     font=panel_font(9), width=16, anchor='w').pack(side='left', padx=1)
            tk.Label(row, text=str(skill['skill_id']), bg=row_bg, fg=_SAO_PANEL_MUTED,
                     font=panel_font(8), width=8, anchor='w').pack(side='left', padx=1)
            tk.Label(row, text=str(slid), bg=row_bg, fg=_SAO_PANEL_MUTED,
                     font=panel_font(8), width=8, anchor='w').pack(side='left', padx=1)

            if skill['has_cd']:
                rem_s = skill['remaining_ms'] / 1000.0
                cd_text = f'{rem_s:.1f}s / {skill["total_cd_ms"]/1000:.0f}s'
                cd_fg = _SAO_PANEL_GOLD
            else:
                cd_text = '就绪'
                cd_fg = _pc('ok', '#3fae5a')
            tk.Label(row, text=cd_text, bg=row_bg, fg=cd_fg,
                     font=panel_font(8), width=10, anchor='w').pack(side='left', padx=1)

            if already:
                tk.Label(row, text='已添加', bg=row_bg, fg=_SAO_PANEL_MUTED,
                         font=panel_font(8), width=8).pack(side='left', padx=1)
            else:
                add_btn = tk.Label(row, text='+ 添加', bg=row_bg,
                                   fg=_SAO_PANEL_ACCENT,
                                   font=panel_font(9, bold=True),
                                   cursor='hand2')
                add_btn.pack(side='left', padx=1)
                add_btn.bind('<Button-1>', lambda _e, s=slid, n=skill['name']:
                             self._add_skill(s, n))

    def _add_skill(self, skill_level_id: int, name: str):
        ok = self.owner._add_custom_monitor(skill_level_id, name=name)
        if ok:
            self.refresh()

    def _remove(self, slot: int):
        self.owner._remove_custom_monitor(slot)
        self.refresh()

    def _toggle_field(self, slot: int, field: str, value: bool):
        self.owner._update_custom_monitor(slot, **{field: value})
        self.refresh()

    def _cycle_parent(self, slot: int, current: int):
        new_parent = (current + 1) % 10
        self.owner._update_custom_monitor(slot, parent_slot=new_parent)
        self.refresh()
