# -*- coding: utf-8 -*-
"""SAOButton / SAOProgressBar / SAOStatusPill / SAOResizeGrip /
SAOSeparator / SAOTitleBar (split from sao_theme.py — verbatim)."""
import tkinter as tk
from config import APP_VERSION_LABEL
from sao_theme.colors import SAOColors

# ──────────────────── SAO 通用按钮 ────────────────────
class SAOButton(tk.Canvas):
    """SAO 风格按钮 (矩形白底, 金色悬停)"""

    def __init__(self, parent, text='', command=None,
                 width=120, height=36, **kw):
        parent_bg = '#0a0e14'
        try:
            parent_bg = parent.cget('bg')
        except Exception:
            pass
        super().__init__(parent, width=width, height=height,
                         highlightthickness=0, cursor='hand2',
                         bg=parent_bg, **kw)
        self.text = text
        self.command = command
        self._btn_w = width
        self._btn_h = height
        self._hovering = False

        self._draw()

        self.bind('<Enter>', self._on_enter)
        self.bind('<Leave>', self._on_leave)
        self.bind('<Button-1>', self._on_click)

    def _draw(self):
        self.delete('all')
        if self._hovering:
            fill = SAOColors.CHILD_HOVER
            fg = '#ffffff'
        else:
            fill = '#ffffff'
            fg = '#333333'

        self.create_rectangle(0, 0, self._btn_w, self._btn_h, fill=fill, outline='#c9c6c6')
        self.create_text(self._btn_w // 2, self._btn_h // 2, text=self.text,
                         fill=fg, font=('Microsoft YaHei UI', 10))

    def set_text(self, text):
        self.text = text
        self._draw()

    def _on_enter(self, e=None):
        self._hovering = True
        self._draw()

    def _on_leave(self, e=None):
        self._hovering = False
        self._draw()

    def _on_click(self, e=None):
        if self.command:
            self.command()


# ──────────────────── SAO 进度条 / 状态 ────────────────────
class SAOProgressBar(tk.Canvas):
    """SAO 风格进度条 (HP 条简化版，嵌入式)"""

    def __init__(self, parent, width=300, height=20, **kw):
        parent_bg = '#0a0e14'
        try:
            parent_bg = parent.cget('bg')
        except Exception:
            pass
        super().__init__(parent, width=width, height=height,
                         highlightthickness=0, bg=parent_bg, **kw)
        self._bar_w = width
        self._bar_h = height
        self._value = 0.0
        self._draw()

    def set_value(self, v: float):
        self._value = max(0.0, min(1.0, v))
        self._draw()

    def _draw(self):
        self.delete('all')
        w, h = self._bar_w, self._bar_h
        self.create_rectangle(0, 0, w, h, fill='#1a2535', outline='#2a4a5e')
        fw = int(w * self._value)
        if fw > 0:
            if self._value > 0.5:
                c = '#9ad334'
            elif self._value > 0.25:
                c = '#f4fa49'
            else:
                c = '#ef684e'
            self.create_rectangle(1, 1, fw, h - 1, fill=c, outline='')
        self.create_text(w // 2, h // 2, text=f'{int(self._value * 100)}%',
                         fill='#e8f4f8', font=('Segoe UI', 8))


class SAOStatusPill(tk.Canvas):
    """SAO 风格状态指示器"""

    def __init__(self, parent, text='Ready', color='#4caf50',
                 width=100, height=24, **kw):
        parent_bg = '#0a0e14'
        try:
            parent_bg = parent.cget('bg')
        except Exception:
            pass
        super().__init__(parent, width=width, height=height,
                         highlightthickness=0, bg=parent_bg, **kw)
        self._text = text
        self._color = color
        self._pill_w = width
        self._pill_h = height
        self._draw()

    def set_status(self, text: str, color: str = None):
        self._text = text
        if color:
            self._color = color
        self._draw()

    def _draw(self):
        self.delete('all')
        w, h = self._pill_w, self._pill_h
        self.create_rectangle(0, 0, w, h, fill='#111820', outline='#2a4a5e')
        self.create_rectangle(2, 2, 8, h - 2, fill=self._color, outline='')
        self.create_text(w // 2 + 3, h // 2, text=self._text,
                         fill='#e8f4f8', font=('Segoe UI', 8))


class SAOResizeGrip(tk.Canvas):
    """SAO 风格调整大小手柄"""

    def __init__(self, parent, root, size=16, **kw):
        super().__init__(parent, width=size, height=size,
                         highlightthickness=0, cursor='size_nw_se', **kw)
        self.root = root
        self._size = size
        self.configure(bg=parent.cget('bg'))
        self._draw()
        self.bind('<Button-1>', self._start)
        self.bind('<B1-Motion>', self._resize)

    def _draw(self):
        self.delete('all')
        s = self._size
        for i in range(3):
            offset = s - 4 - i * 5
            self.create_polygon(offset, s, s, offset, s, s,
                                fill='#4de8f4', outline='')

    def _start(self, e):
        self._sx = e.x_root
        self._sy = e.y_root
        self._sw = self.root.winfo_width()
        self._sh = self.root.winfo_height()

    def _resize(self, e):
        dx = e.x_root - self._sx
        dy = e.y_root - self._sy
        w = max(400, self._sw + dx)
        h = max(300, self._sh + dy)
        self.root.geometry(f'{w}x{h}')


class SAOSeparator(tk.Canvas):
    """SAO 风格分隔线"""

    def __init__(self, parent, width=200, **kw):
        super().__init__(parent, width=width, height=2,
                         highlightthickness=0, **kw)
        self.configure(bg=parent.cget('bg'))
        self.create_line(0, 1, width, 1, fill='#2a4a5e', width=1)


# ──────────────────── SAO 标题栏 ────────────────────
class SAOTitleBar(tk.Frame):
    """SAO 风格标题栏"""

    def __init__(self, parent, root, title="咲 ACT UI",
                 version=APP_VERSION_LABEL, on_close=None, **kw):
        super().__init__(parent, bg='#080c12', height=36, **kw)
        self.root = root
        self.on_close = on_close
        self.pack_propagate(False)

        # 菱形图标
        icon_cv = tk.Canvas(self, width=12, height=12, bg='#080c12',
                            highlightthickness=0)
        icon_cv.create_polygon(6, 0, 12, 6, 6, 12, 0, 6,
                               fill='#4de8f4', outline='')
        icon_cv.pack(side=tk.LEFT, padx=(12, 6), pady=12)

        self._title_lbl = tk.Label(self, text=title, bg='#080c12',
                                   fg='#4de8f4',
                                   font=('Segoe UI', 10, 'bold'))
        self._title_lbl.pack(side=tk.LEFT)

        self._version_lbl = tk.Label(self, text=version, bg='#080c12',
                                     fg='#3d6070',
                                     font=('Segoe UI', 8))
        self._version_lbl.pack(side=tk.LEFT, padx=(6, 0))

        self._ctrl_btns = []
        for txt, cmd in [('×', on_close), ('—', self._minimize), ('□', self._maximize)]:
            cv = tk.Canvas(self, width=28, height=28, bg='#080c12',
                           highlightthickness=0, cursor='hand2')
            cv.create_text(14, 14, text=txt, fill='#3d6070',
                           font=('Consolas', 12))
            cv.pack(side=tk.RIGHT, padx=2, pady=4)
            if cmd:
                cv.bind('<Button-1>', lambda e, c=cmd: c())
            cv.bind('<Enter>', lambda e, c=cv: c.itemconfig('all', fill='#4de8f4'))
            cv.bind('<Leave>', lambda e, c=cv: c.itemconfig('all', fill='#3d6070'))
            self._ctrl_btns.append((cv, txt))

        self._drag = {'x': 0, 'y': 0}
        for w in [self, self._title_lbl, self._version_lbl, icon_cv]:
            w.bind('<Button-1>', self._start_drag)
            w.bind('<B1-Motion>', self._on_drag)
            w.bind('<Double-Button-1>', self._maximize)

    def _start_drag(self, e):
        self._drag['x'] = e.x_root
        self._drag['y'] = e.y_root

    def _on_drag(self, e):
        dx = e.x_root - self._drag['x']
        dy = e.y_root - self._drag['y']
        self.root.geometry(f'+{self.root.winfo_x() + dx}+{self.root.winfo_y() + dy}')
        self._drag['x'] = e.x_root
        self._drag['y'] = e.y_root

    def _minimize(self):
        self.root.overrideredirect(False)
        self.root.iconify()
        self.root.after(100, lambda: self.root.overrideredirect(True))

    def _maximize(self, e=None):
        if self.root.state() == 'zoomed':
            self.root.state('normal')
        else:
            self.root.state('zoomed')

