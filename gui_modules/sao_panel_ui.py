# -*- coding: utf-8 -*-
"""
SAO-style floating panel UI primitives — extracted from sao_gui.py
in round 49 of the sao_gui split refactor.

The constants and helpers in this module are used by every floating
panel that lives on the SAO HUD: the status panel, the AutoKey /
BossRaid panels (via their detail editors), the Commander panel,
the updater status panel, etc. Pulling them into a focused utility
module:

  1. removes 10+ module-level constants + 8 helper functions from
     sao_gui.py (~190 lines), and
  2. unblocks future mixin extractions of panel handlers that
     reference these helpers (e.g. ``_toggle_status_panel``) —
     those previously could not move out because importing the
     helpers from ``sao_gui`` would have been a circular import.

Contents:
  * Constants: ``_SAO_PANEL_BG``, ``_SAO_PANEL_HEADER_BG``,
    ``_SAO_PANEL_HEADER_FG``, ``_SAO_PANEL_BORDER``,
    ``_SAO_PANEL_ACCENT``, ``_SAO_PANEL_GOLD``, ``_SAO_PANEL_SEP``,
    ``_SAO_PANEL_BODY_BG``, ``_SAO_PANEL_LABEL_FG``,
    ``_SAO_PANEL_VALUE_FG``
  * Helpers: ``_apply_panel_style``, ``_hex_rgba``,
    ``_make_panel_close_button``, ``_sao_panel_header``,
    ``_bind_panel_drag``, ``_sao_panel_body``,
    ``_sao_panel_hud_canvas``, ``_sao_row``, ``_sao_pill``
  * Internal state: ``_close_btn_photo_cache`` (PhotoImage cache
    keyed by ``f'{size}_{bg}'`` so multiple panels reuse the same
    Image objects)
"""

from __future__ import annotations

import ctypes
import tkinter as tk
from typing import Any, Dict, Tuple

from PIL import Image, ImageDraw, ImageTk

from sao_sound import get_sao_font, get_cjk_font


# ── Win32 user32 handle (same as sao_gui's _user32 but local here so
# this module is self-contained and importable from anywhere) ──
_user32 = ctypes.windll.user32


def _apply_panel_style(panel):
    """为浮动 Toplevel 面板添加 DWM 圆角 — 增强浮动质感"""
    try:
        panel.update_idletasks()
        hwnd = int(_user32.GetParent(ctypes.c_void_p(panel.winfo_id())))
        # DWM 圆角 (Win11+)
        val = ctypes.c_int(2)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(val), 4)
        # 注：不再设置 CS_DROPSHADOW — 它修改窗口类样式，会污染同进程所有
        # Toplevel（包括 ULW overlay），导致与 PIL 自绘 shadow 双重叠加偏移。
    except Exception:
        pass


# ── SAO HUD 面板样式常量 ──
_SAO_PANEL_BG = '#fafafa'          # 面板主背景
_SAO_PANEL_HEADER_BG = '#1a2030'   # 深色标题栏
_SAO_PANEL_HEADER_FG = '#e8f4f8'   # 标题文字
_SAO_PANEL_BORDER = '#d1d1d6'      # 外边框
_SAO_PANEL_ACCENT = '#86dfff'      # 青色强调
_SAO_PANEL_GOLD = '#f3af12'        # 金色强调
_SAO_PANEL_SEP = '#e0e0e0'         # 分隔线
_SAO_PANEL_BODY_BG = '#ffffff'     # 内容区背景
_SAO_PANEL_LABEL_FG = '#999999'    # 标签文字
_SAO_PANEL_VALUE_FG = '#333333'    # 数值文字


def _hex_rgba(hex_color: str, alpha: int = 255):
    hex_color = hex_color.lstrip('#')
    if len(hex_color) == 3:
        hex_color = ''.join(ch * 2 for ch in hex_color)
    return tuple(int(hex_color[i:i+2], 16) for i in (0, 2, 4)) + (alpha,)


# ── PhotoImage cache for panel close buttons (avoid per-panel re-creation) ──
_close_btn_photo_cache: Dict[str, Tuple[Any, Any]] = {}


def _make_panel_close_button(parent, command, bg=_SAO_PANEL_HEADER_BG):
    size = 18
    scale = 4
    sw = size * scale
    cache_key = f'{size}_{bg}'
    cached = _close_btn_photo_cache.get(cache_key)
    if cached is not None:
        normal, hover = cached
    else:
        img = Image.new('RGBA', (sw, sw), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        def S(v):
            return int(round(v * scale))

        draw.line((S(4), S(4), S(14), S(14)), fill=_hex_rgba('#ff707a'), width=max(1, S(2)))
        draw.line((S(4), S(14), S(14), S(4)), fill=_hex_rgba('#ff707a'), width=max(1, S(2)))
        normal = ImageTk.PhotoImage(img.resize((size, size), Image.LANCZOS))

        img_h = Image.new('RGBA', (sw, sw), (0, 0, 0, 0))
        draw_h = ImageDraw.Draw(img_h)
        draw_h.ellipse((S(1), S(1), S(17), S(17)), outline=_hex_rgba('#ff707a', 210), width=max(1, S(1)))
        draw_h.line((S(4), S(4), S(14), S(14)), fill=_hex_rgba('#ffffff'), width=max(1, S(2)))
        draw_h.line((S(4), S(14), S(14), S(4)), fill=_hex_rgba('#ffffff'), width=max(1, S(2)))
        hover = ImageTk.PhotoImage(img_h.resize((size, size), Image.LANCZOS))
        _close_btn_photo_cache[cache_key] = (normal, hover)

    lbl = tk.Label(parent, bg=bg, image=normal, cursor='hand2', bd=0, highlightthickness=0)
    lbl._img_normal = normal
    lbl._img_hover = hover
    lbl.configure(image=normal)
    lbl.bind('<Enter>', lambda e: lbl.configure(image=lbl._img_hover))
    lbl.bind('<Leave>', lambda e: lbl.configure(image=lbl._img_normal))
    lbl.bind('<Button-1>', lambda e: command())
    return lbl


def _sao_panel_header(parent, title_icon, title_text, close_cmd):
    """创建 SAO 风格深色标题栏，返回 (header_frame, close_label)"""
    hdr = tk.Frame(parent, bg=_SAO_PANEL_HEADER_BG, height=28)
    hdr.pack(fill=tk.X)
    hdr.pack_propagate(False)
    # 左侧角标 + 标题
    accent = tk.Frame(hdr, bg=_SAO_PANEL_ACCENT, width=3, height=16)
    accent.pack(side=tk.LEFT, padx=(6, 0), pady=6)
    tk.Label(hdr, text=f'{title_icon} {title_text}',
             bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_HEADER_FG,
             font=get_sao_font(8, True)).pack(side=tk.LEFT, padx=6)
    # 右侧系统标记
    tk.Label(hdr, text='◇', bg=_SAO_PANEL_HEADER_BG, fg='#4a5a6a',
             font=get_sao_font(7)).pack(side=tk.RIGHT, padx=(0, 2))
    close_lbl = _make_panel_close_button(hdr, close_cmd, bg=_SAO_PANEL_HEADER_BG)
    close_lbl.pack(side=tk.RIGHT, padx=6)
    return hdr, close_lbl


def _bind_panel_drag(hdr, close_lbl, start_fn, move_fn):
    """递归绑定拖拽事件到标题栏的所有子组件 (排除关闭按钮)"""
    def _do(w):
        if w is close_lbl:
            return
        w.bind('<Button-1>', start_fn)
        w.bind('<B1-Motion>', move_fn)
        for ch in w.winfo_children():
            _do(ch)
    _do(hdr)


def _sao_panel_body(parent):
    """创建 SAO 风格面板内容区 (带角标装饰)"""
    # 分隔线
    tk.Frame(parent, bg=_SAO_PANEL_ACCENT, height=1).pack(fill=tk.X)
    body = tk.Frame(parent, bg=_SAO_PANEL_BODY_BG)
    body.pack(fill=tk.BOTH, expand=True, padx=1, pady=(0, 1))
    return body


def _sao_panel_hud_canvas(parent):
    """在面板底部添加一个 HUD 装饰画布层"""
    cv = tk.Canvas(parent, height=16, bg=_SAO_PANEL_BODY_BG,
                   highlightthickness=0, bd=0)
    cv.pack(fill=tk.X, side=tk.BOTTOM)
    return cv


def _sao_row(parent, label_text, value_text='', value_fg=None, value_font=None):
    """创建 SAO 风格的 标签: 值 行"""
    row = tk.Frame(parent, bg=_SAO_PANEL_BODY_BG)
    row.pack(fill=tk.X, pady=2)
    tk.Label(row, text=label_text, bg=_SAO_PANEL_BODY_BG,
             fg=_SAO_PANEL_LABEL_FG, font=get_sao_font(8),
             anchor='w').pack(side=tk.LEFT)
    val_lbl = tk.Label(row, text=value_text, bg=_SAO_PANEL_BODY_BG,
                        fg=value_fg or _SAO_PANEL_VALUE_FG,
                        font=value_font or get_sao_font(9, True))
    val_lbl.pack(side=tk.RIGHT)
    return val_lbl


def _sao_pill(parent, text, active, command):
    """创建 SAO 风格切换按钮"""
    bg = _SAO_PANEL_GOLD if active else '#1a2030'
    fg = '#ffffff' if active else '#8a9aaa'
    lbl = tk.Label(parent, text=text, bg=bg, fg=fg,
                   font=get_cjk_font(8, True),
                   padx=8, pady=2, cursor='hand2', relief=tk.FLAT)
    lbl.bind('<Button-1>', lambda e: command())
    return lbl
