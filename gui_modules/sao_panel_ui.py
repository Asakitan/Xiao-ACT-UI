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
import os
import sys
import tkinter as tk
from typing import Any, Dict, Tuple

from PIL import Image, ImageDraw, ImageTk

from config import resource_path
from utils.sao_sound import get_sao_font, get_cjk_font


# ── Win32 user32 handle (same as sao_gui's _user32 but local here so
# this module is self-contained and importable from anywhere) ──
_user32 = ctypes.windll.user32


def _get_icon_path():
    """Locate the runtime icon.ico path; returns None if missing."""
    p = resource_path('icon.ico')
    return p if os.path.exists(p) else None


def _set_process_app_id(app_id: str):
    """Set the Windows AppUserModelID for the current process so taskbar
    grouping uses our app-specific identity instead of the python.exe
    default. Best-effort; silently no-ops on non-Windows or when the
    shell32 call fails (older Windows versions etc.).
    """
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
    except Exception:
        pass


def _apply_window_icon(win):
    """Apply the runtime icon.ico to a Tk Toplevel (both the title bar
    bitmap via iconbitmap and the taskbar HICON via Win32 WM_SETICON
    so the icon shows up in Alt+Tab / taskbar even for overrideredirect
    Toplevels)."""
    icon_path = _get_icon_path()
    if not icon_path:
        return
    try:
        win.iconbitmap(default=icon_path)
        win.iconbitmap(icon_path)
    except Exception:
        pass
    try:
        win.update_idletasks()
        hwnd = int(_user32.GetParent(ctypes.c_void_p(win.winfo_id())))
        if not hwnd:
            return
        IMAGE_ICON = 1
        LR_LOADFROMFILE = 0x10
        LR_DEFAULTSIZE = 0x40
        WM_SETICON = 0x80
        hicon = _user32.LoadImageW(None, icon_path, IMAGE_ICON, 0, 0,
                                   LR_LOADFROMFILE | LR_DEFAULTSIZE)
        if hicon:
            if not hasattr(win, '_taskbar_hicons'):
                win._taskbar_hicons = []
            win._taskbar_hicons.append(hicon)
            _user32.SendMessageW(ctypes.c_void_p(hwnd), WM_SETICON, 0, hicon)
            _user32.SendMessageW(ctypes.c_void_p(hwnd), WM_SETICON, 1, hicon)
    except Exception:
        pass


def _set_clickthrough_style(win):
    """给装饰/条带窗口设置 Win32 透明点击穿透样式。"""
    try:
        user32 = ctypes.windll.user32
        GWL_EXSTYLE = -20
        WS_EX_LAYERED = 0x00080000
        WS_EX_TRANSPARENT = 0x00000020
        WS_EX_TOOLWINDOW = 0x00000080
        hwnd = user32.GetParent(win.winfo_id()) or win.winfo_id()
        style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        style |= (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)
        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style)
    except Exception:
        pass


def _disable_native_window_shadow(win):
    """关闭透明/异形窗口的系统矩形阴影，避免阴影落到错误区域。"""
    try:
        win.update_idletasks()
        hwnd = int(_user32.GetParent(ctypes.c_void_p(win.winfo_id())) or win.winfo_id())
        policy = ctypes.c_int(1)  # DWMNCRP_DISABLED
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 2, ctypes.byref(policy), 4)
    except Exception:
        pass


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
# Keep this palette aligned with ``sao_web_panel_common.py`` and the WebView
# editor panels: light translucent shell, muted graphite text, cyan/gold
# accents.  ACT floating panels use these helpers as their shared chrome.
_SAO_PANEL_BG = '#e8ebee'          # 面板外壳/透明边缘
_SAO_PANEL_HEADER_BG = '#fcfcfc'   # 浅色标题栏
_SAO_PANEL_HEADER_FG = '#646364'   # 标题文字
_SAO_PANEL_BORDER = '#babec4'      # 卡片/壳边线
_SAO_PANEL_ACCENT = '#68e4ff'      # 青色强调
_SAO_PANEL_GOLD = '#dea620'        # 金色强调
_SAO_PANEL_SEP = '#d8dde2'         # 分隔线
_SAO_PANEL_BODY_BG = '#f6f7f7'     # 内容区背景
_SAO_PANEL_LABEL_FG = '#8c878a'    # 标签文字
_SAO_PANEL_VALUE_FG = '#646364'    # 数值文字

_SAO_PANEL_THEME = 'light'
_SAO_PANEL_ROOTS = []
_SAO_PANEL_CONSTANT_NAMES = (
    '_SAO_PANEL_BG',
    '_SAO_PANEL_HEADER_BG',
    '_SAO_PANEL_HEADER_FG',
    '_SAO_PANEL_BORDER',
    '_SAO_PANEL_ACCENT',
    '_SAO_PANEL_GOLD',
    '_SAO_PANEL_SEP',
    '_SAO_PANEL_BODY_BG',
    '_SAO_PANEL_LABEL_FG',
    '_SAO_PANEL_VALUE_FG',
)
_SAO_PANEL_PALETTES = {
    'light': {
        'bg': '#e8ebee',
        'header_bg': '#fcfcfc',
        'header_fg': '#646364',
        'border': '#babec4',
        'accent': '#68e4ff',
        'accent_strong': '#16a9d6',   # 青色描边/文字（强对比，浅底可读）
        'gold': '#dea620',
        'sep': '#d8dde2',
        'body_bg': '#eef1f4',         # 内容区背景（略深，衬托卡片）
        'card_bg': '#ffffff',         # 卡片背景（抬升）
        'card_bg_alt': '#f4f6f8',     # 斑马行/嵌套
        'label_fg': '#8c878a',
        'value_fg': '#3b3a3c',        # 数值文字（加深，更醒目）
        'control_bg': '#fafbfb',
        'track_bg': '#e2e6ea',        # 进度条底槽
        'accent_soft': '#e2f6fd',     # 青色淡底（徽章/标签）
        'gold_soft': '#fbf2d8',
        'danger': '#ef684e',
        'danger_soft': '#fdeae6',
        'ok': '#3fae5a',
        'ok_soft': '#e6f6ea',
        'warn_soft': '#fff8e5',
        'active_fg': '#ffffff',
    },
    'dark': {
        'bg': '#14202b',
        'header_bg': '#111b28',
        'header_fg': '#e6f4ff',
        'border': '#2d5e6f',
        'accent': '#68e4ff',
        'accent_strong': '#7fe9ff',
        'gold': '#f0c456',
        'sep': '#1c3743',
        'body_bg': '#101823',         # 内容区背景（对齐 webref shell 主色）
        'card_bg': '#162233',         # 卡片背景（抬升）
        'card_bg_alt': '#1a283b',     # 斑马行/嵌套
        'label_fg': '#9fb4c4',
        'value_fg': '#eaf6ff',
        'control_bg': '#172436',
        'track_bg': '#224253',        # 进度条底槽
        'accent_soft': '#15303f',     # 青色淡底（徽章/标签）
        'gold_soft': '#2f2916',
        'danger': '#ff707a',
        'danger_soft': '#311c22',
        'ok': '#7df2bf',
        'ok_soft': '#16322a',
        'warn_soft': '#2c2617',
        'active_fg': '#0c141f',
    },
}


def _normalize_sao_panel_theme(theme: str) -> str:
    return 'light' if str(theme or '').lower() == 'light' else 'dark'


def _sync_imported_sao_panel_constants() -> None:
    values = {
        '_SAO_PANEL_BG': _SAO_PANEL_BG,
        '_SAO_PANEL_HEADER_BG': _SAO_PANEL_HEADER_BG,
        '_SAO_PANEL_HEADER_FG': _SAO_PANEL_HEADER_FG,
        '_SAO_PANEL_BORDER': _SAO_PANEL_BORDER,
        '_SAO_PANEL_ACCENT': _SAO_PANEL_ACCENT,
        '_SAO_PANEL_GOLD': _SAO_PANEL_GOLD,
        '_SAO_PANEL_SEP': _SAO_PANEL_SEP,
        '_SAO_PANEL_BODY_BG': _SAO_PANEL_BODY_BG,
        '_SAO_PANEL_LABEL_FG': _SAO_PANEL_LABEL_FG,
        '_SAO_PANEL_VALUE_FG': _SAO_PANEL_VALUE_FG,
    }
    for name, module in list(sys.modules.items()):
        if module is None or module is sys.modules.get(__name__):
            continue
        if not (
            name == 'sao_gui'
            or name.startswith('sao_gui.')
            or name.startswith('gui_modules.')
        ):
            continue
        if not any(hasattr(module, attr) for attr in _SAO_PANEL_CONSTANT_NAMES):
            continue
        for attr, value in values.items():
            if hasattr(module, attr):
                try:
                    setattr(module, attr, value)
                except Exception:
                    pass


def _apply_sao_panel_palette(theme: str) -> None:
    global _SAO_PANEL_THEME
    global _SAO_PANEL_BG, _SAO_PANEL_HEADER_BG, _SAO_PANEL_HEADER_FG
    global _SAO_PANEL_BORDER, _SAO_PANEL_ACCENT, _SAO_PANEL_GOLD
    global _SAO_PANEL_SEP, _SAO_PANEL_BODY_BG, _SAO_PANEL_LABEL_FG
    global _SAO_PANEL_VALUE_FG

    _SAO_PANEL_THEME = _normalize_sao_panel_theme(theme)
    p = _SAO_PANEL_PALETTES[_SAO_PANEL_THEME]
    _SAO_PANEL_BG = p['bg']
    _SAO_PANEL_HEADER_BG = p['header_bg']
    _SAO_PANEL_HEADER_FG = p['header_fg']
    _SAO_PANEL_BORDER = p['border']
    _SAO_PANEL_ACCENT = p['accent']
    _SAO_PANEL_GOLD = p['gold']
    _SAO_PANEL_SEP = p['sep']
    _SAO_PANEL_BODY_BG = p['body_bg']
    _SAO_PANEL_LABEL_FG = p['label_fg']
    _SAO_PANEL_VALUE_FG = p['value_fg']
    _sync_imported_sao_panel_constants()


_apply_sao_panel_palette(_SAO_PANEL_THEME)


def _normalise_hex_color(value: Any) -> str:
    try:
        return str(value or '').strip().lower()
    except Exception:
        return ''


def _semantic_color_key(value: Any) -> str:
    color = _normalise_hex_color(value)
    if not color:
        return ''
    for palette in _SAO_PANEL_PALETTES.values():
        for key, candidate in palette.items():
            if _normalise_hex_color(candidate) == color:
                return key
    return ''


def _theme_color(key: str, fallback: str = '') -> str:
    return _SAO_PANEL_PALETTES[_SAO_PANEL_THEME].get(key, fallback)


def _remember_sao_panel_root(root) -> None:
    try:
        if root is None:
            return
        for existing in list(_SAO_PANEL_ROOTS):
            try:
                if existing is root:
                    return
                if not existing.winfo_exists():
                    _SAO_PANEL_ROOTS.remove(existing)
            except Exception:
                try:
                    _SAO_PANEL_ROOTS.remove(existing)
                except Exception:
                    pass
        _SAO_PANEL_ROOTS.append(root)
    except Exception:
        pass


def _apply_sao_theme_to_widget(widget) -> None:
    cls = widget.winfo_class()
    try:
        bg_key = _semantic_color_key(widget.cget('bg'))
    except Exception:
        bg_key = ''
    try:
        fg_key = _semantic_color_key(widget.cget('fg'))
    except Exception:
        fg_key = ''
    try:
        if cls in {'Toplevel'}:
            widget.configure(bg=_SAO_PANEL_BG)
        elif cls in {'Frame', 'Labelframe'}:
            if bg_key:
                widget.configure(bg=_theme_color(bg_key))
            try:
                hb_key = _semantic_color_key(widget.cget('highlightbackground'))
                if hb_key:
                    widget.configure(highlightbackground=_theme_color(hb_key))
                hc_key = _semantic_color_key(widget.cget('highlightcolor'))
                if hc_key:
                    widget.configure(highlightcolor=_theme_color(hc_key))
            except Exception:
                pass
        elif cls == 'Label':
            updates = {}
            if bg_key:
                updates['bg'] = _theme_color(bg_key)
            if fg_key:
                updates['fg'] = _theme_color(fg_key)
            if updates:
                widget.configure(**updates)
            try:
                hb_key = _semantic_color_key(widget.cget('highlightbackground'))
                if hb_key:
                    widget.configure(highlightbackground=_theme_color(hb_key))
            except Exception:
                pass
    except Exception:
        pass


def _style_sao_panel_tree(root) -> None:
    try:
        _remember_sao_panel_root(root)
        root.configure(bg=_SAO_PANEL_BG)
    except Exception:
        pass
    try:
        _apply_sao_theme_to_widget(root)
    except Exception:
        pass
    try:
        _style_panel_descendants(root)
    except Exception:
        pass


def _set_sao_panel_theme(theme: str, root=None, repaint_registered: bool = False) -> str:
    """Set ACT/Tk SAO panel theme and repaint registered panel roots."""
    _apply_sao_panel_palette(theme)
    targets = [root] if root is not None else (list(_SAO_PANEL_ROOTS) if repaint_registered else [])
    for target in targets:
        try:
            if target is None or not target.winfo_exists():
                continue
            _style_sao_panel_tree(target)
        except Exception:
            pass
    return _SAO_PANEL_THEME


def _enable_frameless_panel(win):
    """Best-effort custom SAO chrome for Toplevel panels."""
    try:
        if isinstance(win, tk.Toplevel):
            _remember_sao_panel_root(win)
            win.overrideredirect(True)
            win.configure(bg=_SAO_PANEL_BG)
            _disable_native_window_shadow(win)
            _apply_panel_style(win)
    except Exception:
        pass


def _hex_rgba(hex_color: str, alpha: int = 255):
    hex_color = hex_color.lstrip('#')
    if len(hex_color) == 3:
        hex_color = ''.join(ch * 2 for ch in hex_color)
    return tuple(int(hex_color[i:i+2], 16) for i in (0, 2, 4)) + (alpha,)


# ── PhotoImage cache for panel close buttons (avoid per-panel re-creation) ──
_close_btn_photo_cache: Dict[str, Tuple[Any, Any]] = {}


def _make_panel_close_button(parent, command, bg=_SAO_PANEL_HEADER_BG, *, flat=False):
    size = 18
    scale = 4
    sw = size * scale
    # flat ACT panels recolor the close-X via the danger token (light #ef684e /
    # dark #ff707a); non-flat callers keep the legacy literal + old cache shape.
    danger = _theme_color('danger', '#ff707a') if flat else '#ff707a'
    cache_key = f'{size}_{bg}_{danger}' if flat else f'{size}_{bg}'
    cached = _close_btn_photo_cache.get(cache_key)
    if cached is not None:
        normal, hover = cached
    else:
        img = Image.new('RGBA', (sw, sw), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        def S(v):
            return int(round(v * scale))

        draw.line((S(4), S(4), S(14), S(14)), fill=_hex_rgba(danger), width=max(1, S(2)))
        draw.line((S(4), S(14), S(14), S(4)), fill=_hex_rgba(danger), width=max(1, S(2)))
        normal = ImageTk.PhotoImage(img.resize((size, size), Image.LANCZOS))

        img_h = Image.new('RGBA', (sw, sw), (0, 0, 0, 0))
        draw_h = ImageDraw.Draw(img_h)
        draw_h.ellipse((S(1), S(1), S(17), S(17)), outline=_hex_rgba(danger, 210), width=max(1, S(1)))
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


def _sao_panel_header(parent, title_icon, title_text=None, close_cmd=None, on_close=None, *, flat=False):
    """创建 SAO 风格深色标题栏，返回 header。

    Legacy callers pass ``(parent, icon, title, close_cmd)`` and unpack
    ``(header, close_label)``.  New ACT panels pass ``(parent, title,
    on_close=...)`` and use the returned object as a frame.  Return a small
    tuple-like proxy so both styles stay compatible without duplicating panel
    chrome code.
    """
    if title_text is None:
        title_text = str(title_icon or '')
        title_icon = '◉'
    close_cb = on_close or close_cmd or (lambda: None)
    if flat:
        try:
            _remember_sao_panel_root(parent.winfo_toplevel())
        except Exception:
            pass
        hdr = tk.Canvas(parent, bg=_SAO_PANEL_BG, height=1, bd=0, highlightthickness=0)
        hdr.pack(fill=tk.X)
        close_lbl = tk.Frame(hdr, bg=_SAO_PANEL_BG, width=0, height=0, bd=0, highlightthickness=0)

        def _draw_flat_top(_event=None):
            try:
                width = max(1, hdr.winfo_width())
                hdr.delete('flat-top')
                hdr.create_rectangle(2, 0, max(2, width - 3), 1,
                                     fill=_SAO_PANEL_BORDER, outline='', tags='flat-top')
            except Exception:
                pass

        hdr.bind('<Configure>', _draw_flat_top)
        hdr.after(1, _draw_flat_top)

        class _FlatHeaderProxy:
            def __init__(self, frame, close_button):
                self.frame = frame
                self.close_label = close_button

            def __iter__(self):
                yield self.frame
                yield self.close_label

            def __getattr__(self, name):
                return getattr(self.frame, name)

        return _FlatHeaderProxy(hdr, close_lbl)
    try:
        _enable_frameless_panel(parent.winfo_toplevel())
    except Exception:
        pass
    hdr = tk.Frame(parent, bg=_SAO_PANEL_HEADER_BG, height=34, bd=0, highlightthickness=0)
    hdr.pack(fill=tk.X)
    hdr.pack_propagate(False)
    # 左侧角标 + 标题
    accent = tk.Frame(hdr, bg=_SAO_PANEL_ACCENT, width=3, height=18)
    accent.pack(side=tk.LEFT, padx=(8, 0), pady=8)
    tk.Label(hdr, text=f'{title_icon} {title_text}',
             bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_HEADER_FG,
             font=get_sao_font(9, True)).pack(side=tk.LEFT, padx=(7, 4))
    if not flat:
        # 装饰金条 + 右侧 ◇ 系统标记（扁平 ACT 面板去掉，只留左侧 accent 条 + 标题）
        tk.Frame(hdr, bg=_SAO_PANEL_GOLD, width=24, height=2).pack(side=tk.LEFT, padx=(3, 0), pady=(18, 0))
        tk.Label(hdr, text='◇', bg=_SAO_PANEL_HEADER_BG, fg=_SAO_PANEL_SEP,
                 font=get_sao_font(7)).pack(side=tk.RIGHT, padx=(0, 2))
    close_lbl = _make_panel_close_button(hdr, close_cb, bg=_SAO_PANEL_HEADER_BG, flat=flat)
    close_lbl.pack(side=tk.RIGHT, padx=(6, 8))
    class _HeaderProxy:
        def __init__(self, frame, close_button):
            self.frame = frame
            self.close_label = close_button

        def __iter__(self):
            yield self.frame
            yield self.close_label

        def __getattr__(self, name):
            return getattr(self.frame, name)

    return _HeaderProxy(hdr, close_lbl)


def _bind_panel_drag(hdr, close_lbl=None, start_fn=None, move_fn=None):
    """递归绑定拖拽事件到标题栏的所有子组件 (排除关闭按钮)"""
    drag_root = None
    if start_fn is None and move_fn is None and hasattr(close_lbl, 'frame') and hasattr(close_lbl, 'close_label'):
        drag_root = hdr
        hdr = close_lbl.frame
        close_lbl = close_lbl.close_label
    if hasattr(hdr, 'frame') and hasattr(hdr, 'close_label'):
        close_lbl = hdr.close_label
        hdr = hdr.frame
    if hasattr(close_lbl, 'frame') and hasattr(close_lbl, 'close_label'):
        close_lbl = close_lbl.close_label
    if start_fn is None or move_fn is None:
        root = drag_root or hdr.winfo_toplevel()
        state = {'x': 0, 'y': 0}

        def start_fn(event):
            state['x'] = event.x_root
            state['y'] = event.y_root

        def move_fn(event):
            try:
                dx = event.x_root - state['x']
                dy = event.y_root - state['y']
                state['x'] = event.x_root
                state['y'] = event.y_root
                root.geometry(f'+{root.winfo_x() + dx}+{root.winfo_y() + dy}')
            except Exception:
                pass

    def _do(w):
        if w is close_lbl:
            return
        w.bind('<Button-1>', start_fn)
        w.bind('<B1-Motion>', move_fn)
        for ch in w.winfo_children():
            _do(ch)
    _do(hdr)


def _sao_panel_body(parent, *, flat=False):
    """创建 SAO 风格面板内容区。flat=True 时去掉 2px 青条/角块/焦点辉光（扁平 ACT 面板）。"""
    if not flat:
        tk.Frame(parent, bg=_SAO_PANEL_SEP, height=1).pack(fill=tk.X)
    if not flat:
        tk.Frame(parent, bg=_SAO_PANEL_ACCENT, height=2).pack(fill=tk.X)
    body = tk.Frame(
        parent,
        bg=_SAO_PANEL_BODY_BG,
        highlightthickness=1,
        highlightbackground=_SAO_PANEL_BORDER,
        highlightcolor=_SAO_PANEL_BORDER if flat else _SAO_PANEL_ACCENT,
    )
    body.pack(fill=tk.BOTH, expand=True, padx=0 if flat else 1, pady=0 if flat else (0, 1))
    if not flat:
        try:
            tk.Frame(body, bg=_SAO_PANEL_ACCENT, width=34, height=2).place(x=0, y=0)
            tk.Frame(body, bg=_SAO_PANEL_GOLD, width=34, height=2).place(relx=1.0, rely=1.0, anchor='se')
        except Exception:
            pass
    for delay in (0, 80, 240):
        try:
            body.after(delay, lambda root=body: _style_panel_descendants(root))
        except Exception:
            pass
    return body


def _style_panel_descendants(root):
    """Apply one-shot SAO styling to simple Tk controls created in a panel."""
    try:
        children = list(root.winfo_children())
    except Exception:
        return
    for child in children:
        try:
            if hasattr(child, 'winfo_exists') and not child.winfo_exists():
                continue
            cls = child.winfo_class()
        except Exception:
            continue
        try:
            _apply_sao_theme_to_widget(child)
        except Exception:
            pass
        try:
            if cls == 'Button':
                child.configure(
                    bg=_SAO_PANEL_HEADER_BG,
                    fg=_SAO_PANEL_HEADER_FG,
                    activebackground=_SAO_PANEL_ACCENT,
                    activeforeground=_theme_color('active_fg', '#ffffff'),
                    relief=tk.FLAT,
                    bd=0,
                    padx=max(int(str(child.cget('padx') or 0)), 8),
                    pady=max(int(str(child.cget('pady') or 0)), 3),
                    highlightthickness=1,
                    highlightbackground=_SAO_PANEL_BORDER,
                    highlightcolor=_SAO_PANEL_ACCENT,
                )
            elif cls == 'Entry':
                child.configure(
                    bg=_theme_color('control_bg', '#fafbfb'),
                    fg=_SAO_PANEL_VALUE_FG,
                    insertbackground=_SAO_PANEL_GOLD,
                    relief=tk.FLAT,
                    bd=0,
                    highlightthickness=1,
                    highlightbackground=_SAO_PANEL_BORDER,
                    highlightcolor=_SAO_PANEL_ACCENT,
                )
            elif cls == 'Menubutton':
                child.configure(
                    bg=_theme_color('control_bg', '#fafbfb'),
                    fg=_SAO_PANEL_VALUE_FG,
                    activebackground=_SAO_PANEL_ACCENT,
                    activeforeground=_theme_color('active_fg', '#ffffff'),
                    relief=tk.FLAT,
                    bd=0,
                    highlightthickness=1,
                    highlightbackground=_SAO_PANEL_BORDER,
                    highlightcolor=_SAO_PANEL_ACCENT,
                )
                menu = child.cget('menu')
                if menu:
                    menu_obj = child.nametowidget(menu)
                    menu_obj.configure(
                        bg=_theme_color('control_bg', '#fafbfb'),
                        fg=_SAO_PANEL_VALUE_FG,
                        activebackground=_SAO_PANEL_ACCENT,
                        activeforeground=_theme_color('active_fg', '#ffffff'),
                        relief=tk.FLAT,
                        bd=0,
                    )
            elif cls == 'Scrollbar':
                child.configure(
                    troughcolor=_SAO_PANEL_BODY_BG,
                    bg=_SAO_PANEL_BORDER,
                    activebackground=_SAO_PANEL_ACCENT,
                    relief=tk.FLAT,
                    bd=0,
                    highlightthickness=0,
                )
            elif cls == 'Canvas':
                child.configure(bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
            elif cls in {'Checkbutton', 'Radiobutton'}:
                child.configure(
                    bg=_SAO_PANEL_BODY_BG,
                    fg=_SAO_PANEL_VALUE_FG,
                    selectcolor=_SAO_PANEL_HEADER_BG,
                    activebackground=_SAO_PANEL_BODY_BG,
                    activeforeground=_SAO_PANEL_GOLD,
                    relief=tk.FLAT,
                    bd=0,
                    highlightthickness=0,
                )
        except Exception:
            pass
        # Force SAO/CJK font: any widget still on system default gets CJK font
        try:
            if cls in ('Label', 'Button', 'Checkbutton', 'Radiobutton',
                       'Menubutton', 'Message'):
                import tkinter.font as _tkfont
                try:
                    f = _tkfont.Font(font=child.cget('font'))
                    fam = f.actual('family')
                except Exception:
                    fam = ''
                if fam and 'SAO' not in fam and '筑紫' not in fam and 'ZhuZi' not in fam:
                    child.configure(font=get_cjk_font(9))
        except Exception:
            pass
        _style_panel_descendants(child)


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


def _sao_pill(parent, text, active=None, command=None):
    """创建 SAO 风格切换按钮/徽章。

    Older callers used this as a clickable toggle and passed ``active`` plus
    ``command``.  ACT panels also use it as a static badge, so both arguments
    are optional and the visual state is inferred from common status labels.
    """
    token = str(text or '').upper()
    if active is None:
        active = token in {'ACTIVE', 'ENABLED', 'READY', 'RUNNING', 'OK'} or 'SDK' in token
    bad = token in {'ERROR', 'DISABLED', 'FAILED', 'BAD'}
    warn = token in {'WARN', 'WARNING', 'TIMER', 'PAUSED'}
    if bad:
        bg = _theme_color('danger_soft', '#fff0f2')
        fg = _theme_color('danger', '#ef684e')
        border = _theme_color('danger', '#ef684e')
    elif warn:
        bg = _theme_color('warn_soft', '#fff8e5')
        fg = _SAO_PANEL_GOLD
        border = _SAO_PANEL_GOLD
    elif active:
        bg = _SAO_PANEL_GOLD
        fg = _theme_color('active_fg', '#ffffff')
        border = _SAO_PANEL_GOLD
    else:
        bg = _theme_color('control_bg', '#fafbfb')
        fg = _SAO_PANEL_VALUE_FG
        border = _SAO_PANEL_ACCENT
    lbl = tk.Label(parent, text=text, bg=bg, fg=fg,
                   font=get_cjk_font(8, True),
                   padx=9, pady=3,
                   cursor='hand2' if callable(command) else '',
                   relief=tk.FLAT,
                   highlightthickness=1,
                   highlightbackground=border,
                   highlightcolor=border)
    if callable(command):
        lbl.bind('<Button-1>', lambda e: command())
    return lbl
