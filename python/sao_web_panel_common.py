from __future__ import annotations

import tkinter as tk
from tkinter import font as tkfont
from typing import Callable, Dict, Optional, Tuple


PANEL_BG = '#f6f7f7'
PANEL_BG_ALT = '#eef0f1'
PANEL_EDGE = '#bec4c8'
PANEL_EDGE_STRONG = '#9ea7ad'
PANEL_HEADER = '#f7f8f8'
PANEL_HEADER_ALT = '#fbfbfb'
PANEL_CARD = '#f0f2f3'
PANEL_CARD_ALT = '#fafbfb'
TEXT_MAIN = '#646364'
TEXT_MUTED = '#8c878a'
TEXT_DIM = '#b0adb0'
GOLD = '#dea620'
GOLD_STRONG = '#f3af12'
CYAN = '#68e4ff'
CYAN_SOFT = '#e6f9fd'
DANGER = '#ef684e'
READY = '#8dcfaa'
ACTIVE = '#81d9ec'
COOLDOWN = '#d4b976'
LINE = '#ffffff'
LINE_SOFT = '#f3f5f6'
SHADOW = '#c1c7cc'
SHADOW_DEEP = '#959ea5'


_FONT_CACHE: Dict[Tuple[int, bool, str], tkfont.Font] = {}


def _widget_bg(widget: tk.Misc, fallback: str) -> str:
    try:
        return str(widget.cget('bg'))
    except Exception:
        return fallback


def panel_font(size: int, bold: bool = False,
               family: str = 'Segoe UI') -> tkfont.Font:
    key = (size, bold, family)
    font = _FONT_CACHE.get(key)
    if font is None:
        try:
            font = tkfont.Font(
                family=family,
                size=size,
                weight='bold' if bold else 'normal',
            )
        except Exception:
            font = tkfont.Font(size=size, weight='bold' if bold else 'normal')
        _FONT_CACHE[key] = font
    return font


def calc_panel_geometry(master: tk.Misc, *, min_w: int, min_h: int,
                        width_ratio: float, height_ratio: float,
                        x_ratio: float, y_ratio: float) -> Tuple[int, int, int, int]:
    try:
        sw = int(master.winfo_screenwidth())
        sh = int(master.winfo_screenheight())
    except Exception:
        sw, sh = 1920, 1080
    width = max(min_w, int(min(sw, 1920) * width_ratio))
    height = max(min_h, int(min(sh, 1080) * height_ratio))
    x = max(16, int(sw * x_ratio))
    y = max(0, int(sh * y_ratio))
    return width, height, x, y


def bind_drag(widget: tk.Widget, start_fn: Callable, move_fn: Callable) -> None:
    widget.bind('<Button-1>', start_fn)
    widget.bind('<B1-Motion>', move_fn)
    for child in widget.winfo_children():
        bind_drag(child, start_fn, move_fn)


def clear_frame(frame: tk.Widget) -> None:
    for child in frame.winfo_children():
        child.destroy()


def create_scrollable_area(parent: tk.Widget, bg: str) -> Tuple[tk.Frame, tk.Canvas, tk.Frame]:
    wrap = tk.Frame(parent, bg=bg)
    wrap.pack(fill=tk.BOTH, expand=True)

    canvas = tk.Canvas(wrap, bg=bg, highlightthickness=0, bd=0)
    scrollbar = tk.Scrollbar(
        wrap,
        orient=tk.VERTICAL,
        command=canvas.yview,
        bg=PANEL_CARD_ALT,
        activebackground=PANEL_BG_ALT,
        troughcolor=PANEL_BG,
        bd=0,
        relief=tk.FLAT,
        highlightthickness=0,
        elementborderwidth=0,
        width=11,
    )
    body = tk.Frame(canvas, bg=bg)
    window_id = canvas.create_window((0, 0), window=body, anchor='nw')

    def _sync_scrollregion(_event=None):
        try:
            canvas.configure(scrollregion=canvas.bbox('all'))
        except Exception:
            pass

    def _sync_width(event):
        try:
            canvas.itemconfigure(window_id, width=event.width)
        except Exception:
            pass

    body.bind('<Configure>', _sync_scrollregion)
    canvas.bind('<Configure>', _sync_width)
    canvas.configure(yscrollcommand=scrollbar.set)

    canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

    def _mousewheel(event):
        try:
            canvas.yview_scroll(int(-event.delta / 120), 'units')
        except Exception:
            pass

    canvas.bind_all('<MouseWheel>', _mousewheel, add='+')
    return wrap, canvas, body


def place_corner_accents(parent: tk.Widget, *, size: int = 28,
                         tl_color: str = CYAN, br_color: str = GOLD) -> None:
    top_main = tk.Frame(parent, bg=tl_color, height=2, width=size)
    top_main.place(x=0, y=0)
    top_main.lower()
    left_main = tk.Frame(parent, bg=tl_color, width=2, height=size)
    left_main.place(x=0, y=0)
    left_main.lower()
    top_inner = tk.Frame(parent, bg=LINE_SOFT, height=1, width=max(10, size - 10))
    top_inner.place(x=3, y=3)
    top_inner.lower()
    left_inner = tk.Frame(parent, bg=LINE_SOFT, width=1, height=max(10, size - 10))
    left_inner.place(x=3, y=3)
    left_inner.lower()

    bottom_main = tk.Frame(parent, bg=br_color, height=2, width=size)
    bottom_main.place(relx=1.0, rely=1.0, x=-size, y=-2)
    bottom_main.lower()
    right_main = tk.Frame(parent, bg=br_color, width=2, height=size)
    right_main.place(relx=1.0, rely=1.0, x=-2, y=-size)
    right_main.lower()
    bottom_inner = tk.Frame(parent, bg=LINE_SOFT, height=1, width=max(10, size - 10))
    bottom_inner.place(relx=1.0, rely=1.0, x=-max(10, size - 7), y=-5)
    bottom_inner.lower()
    right_inner = tk.Frame(parent, bg=LINE_SOFT, width=1, height=max(10, size - 10))
    right_inner.place(relx=1.0, rely=1.0, x=-5, y=-max(10, size - 7))
    right_inner.lower()


def apply_surface_chrome(parent: tk.Widget, *, accent: Optional[str] = None,
                         accent_side: str = 'left') -> None:
    top = tk.Frame(parent, bg=LINE, height=1)
    top.place(x=2, y=1, relwidth=1.0, width=-4)
    top.lower()
    mid = tk.Frame(parent, bg=LINE_SOFT, height=1)
    mid.place(x=4, y=3, relwidth=1.0, width=-8)
    mid.lower()
    bottom = tk.Frame(parent, bg=PANEL_EDGE_STRONG, height=1)
    bottom.place(x=2, rely=1.0, y=-2, relwidth=1.0, width=-4)
    bottom.lower()
    if accent:
        if accent_side == 'top':
            main = tk.Frame(parent, bg=accent, height=2)
            main.place(x=0, y=0, relwidth=1.0)
            main.lower()
            inner = tk.Frame(parent, bg=LINE_SOFT, height=1)
            inner.place(x=14, y=3, relwidth=1.0, width=-28)
            inner.lower()
        elif accent_side == 'bottom':
            main = tk.Frame(parent, bg=accent, height=2)
            main.place(x=0, rely=1.0, y=-2, relwidth=1.0)
            main.lower()
        elif accent_side == 'right':
            main = tk.Frame(parent, bg=accent, width=2)
            main.place(relx=1.0, x=-2, y=0, relheight=1.0)
            main.lower()
        else:
            main = tk.Frame(parent, bg=accent, width=2)
            main.place(x=0, y=0, relheight=1.0)
            main.lower()
            inner = tk.Frame(parent, bg=LINE_SOFT, width=1)
            inner.place(x=3, y=4, relheight=1.0, height=-8)
            inner.lower()


_BUTTON_STYLES = {
    'default': {
        'bg': PANEL_CARD_ALT,
        'fg': TEXT_MAIN,
        'border': PANEL_EDGE,
        'hover_bg': '#edf6f8',
        'hover_fg': TEXT_MAIN,
        'hover_border': '#a8dbe6',
    },
    'accent': {
        'bg': GOLD,
        'fg': '#ffffff',
        'border': GOLD_STRONG,
        'hover_bg': GOLD_STRONG,
        'hover_fg': '#ffffff',
        'hover_border': GOLD_STRONG,
    },
    'danger': {
        'bg': DANGER,
        'fg': '#ffffff',
        'border': DANGER,
        'hover_bg': '#f0785f',
        'hover_fg': '#ffffff',
        'hover_border': DANGER,
    },
    'ready': {
        'bg': '#e0f5e7',
        'fg': TEXT_MAIN,
        'border': '#97d8b0',
        'hover_bg': '#d0efda',
        'hover_fg': TEXT_MAIN,
        'hover_border': '#7fc49a',
    },
}


def set_action_button_kind(label: tk.Label, kind: str, *, text: Optional[str] = None) -> None:
    palette = _BUTTON_STYLES.get(kind, _BUTTON_STYLES['default'])
    label._button_kind = kind
    label._button_palette = palette
    label.configure(
        text=text if text is not None else label.cget('text'),
        bg=palette['bg'],
        fg=palette['fg'],
        highlightbackground=palette['border'],
        highlightcolor=palette['border'],
    )


def _button_on_enter(label: tk.Label) -> None:
    palette = getattr(label, '_button_palette', _BUTTON_STYLES['default'])
    label.configure(
        bg=palette['hover_bg'],
        fg=palette['hover_fg'],
        highlightbackground=palette['hover_border'],
        highlightcolor=palette['hover_border'],
    )


def _button_on_leave(label: tk.Label) -> None:
    palette = getattr(label, '_button_palette', _BUTTON_STYLES['default'])
    label.configure(
        bg=palette['bg'],
        fg=palette['fg'],
        highlightbackground=palette['border'],
        highlightcolor=palette['border'],
    )


def make_action_button(parent: tk.Widget, text: str, command: Callable[[], None],
                       *, kind: str = 'default', width: int = 0) -> tk.Label:
    label = tk.Label(
        parent,
        text=text,
        font=panel_font(9, bold=True),
        padx=12,
        pady=7,
        cursor='hand2',
        highlightthickness=1,
        bd=0,
        relief=tk.FLAT,
        width=width,
    )
    set_action_button_kind(label, kind)
    label.bind('<Button-1>', lambda _event: command())
    label.bind('<Enter>', lambda _event: _button_on_enter(label))
    label.bind('<Leave>', lambda _event: _button_on_leave(label))
    return label


def make_tab_label(parent: tk.Widget, text: str, command: Callable[[], None]) -> tk.Label:
    base_bg = _widget_bg(parent, PANEL_BG_ALT)
    label = tk.Label(
        parent,
        text=text,
        bg=base_bg,
        fg=TEXT_MUTED,
        font=panel_font(10, bold=True),
        padx=16,
        pady=8,
        cursor='hand2',
    )
    label._tab_base_bg = base_bg
    label.bind('<Button-1>', lambda _event: command())
    label.bind('<Enter>', lambda _event: label.configure(fg=TEXT_MAIN))
    label.bind('<Leave>', lambda _event: set_tab_active(label, getattr(label, '_tab_active', False)))
    return label


def set_tab_active(label: tk.Label, active: bool) -> None:
    label._tab_active = active
    label.configure(fg=GOLD_STRONG if active else TEXT_MUTED,
                    bg=getattr(label, '_tab_base_bg', label.cget('bg')))
    underline = getattr(label, '_underline', None)
    if underline is not None:
        underline.configure(bg=GOLD_STRONG if active else getattr(label, '_tab_base_bg', PANEL_BG_ALT), height=2)


def attach_tab_underline(label: tk.Label, parent: tk.Widget) -> tk.Frame:
    underline = tk.Frame(parent, bg=_widget_bg(parent, PANEL_BG_ALT), height=2)
    underline.pack(fill=tk.X)
    label._underline = underline
    return underline


def make_section_title(parent: tk.Widget, text: str) -> tk.Frame:
    bg = _widget_bg(parent, PANEL_BG)
    row = tk.Frame(parent, bg=bg)
    row.pack(fill=tk.X, pady=(8, 6))
    accent = tk.Frame(row, bg=CYAN, width=14, height=2)
    accent.pack(side=tk.LEFT, padx=(0, 8), pady=7)
    accent.pack_propagate(False)
    tk.Label(
        row,
        text=text,
        bg=bg,
        fg=TEXT_MUTED,
        font=panel_font(9, bold=True),
    ).pack(side=tk.LEFT)
    divider = tk.Frame(row, bg=bg)
    divider.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(10, 0), pady=8)
    tk.Frame(divider, bg=PANEL_EDGE_STRONG, height=1).pack(fill=tk.X)
    tk.Frame(divider, bg=LINE_SOFT, height=1).pack(fill=tk.X, padx=18, pady=(1, 0))
    return row


def apply_badge(label: tk.Label, text: str, kind: str) -> None:
    bg = PANEL_EDGE_STRONG
    fg = '#ffffff'
    border = PANEL_EDGE_STRONG
    if kind == 'running':
        bg = DANGER
        border = DANGER
    elif kind == 'on':
        bg = READY
        fg = TEXT_MAIN
        border = READY
    elif kind == 'off':
        bg = PANEL_EDGE_STRONG
        border = PANEL_EDGE_STRONG
    elif kind == 'active':
        bg = GOLD
        border = GOLD_STRONG
    label.configure(
        text=text,
        bg=bg,
        fg=fg,
        highlightthickness=1,
        highlightbackground=border,
        highlightcolor=border,
        padx=8,
        pady=2,
    )
