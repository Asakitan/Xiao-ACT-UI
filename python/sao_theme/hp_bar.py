# -*- coding: utf-8 -*-
"""SAOHPBar (split from sao_theme.py — verbatim)."""
import tkinter as tk
from utils.sao_sound import get_sao_font as _sao_font
from sao_theme.animator import Animator
from sao_theme.utils import lerp

# ──────────────────── HP 血条 ────────────────────
class SAOHPBar(tk.Canvas):
    """
    SAO Utils 风格 HP 条
    - 左侧缺口方块
    - 用户名标签
    - HP 数值 + Lv 等级
    - 绿/黄/红 渐变条
    - SVG polygon 风格边框
    """

    def __init__(self, parent, username='Player', current=100, total=100,
                 level=1, width=400, height=40, **kw):
        parent_bg = '#0a0e14'
        try:
            parent_bg = parent.cget('bg')
        except Exception:
            pass
        super().__init__(parent, width=width, height=height,
                         highlightthickness=0, bg=parent_bg, **kw)
        self.username = username
        self._current = current
        self._total = total
        self._level = level
        self._hp_w = width
        self._hp_h = height
        self._display_current = current
        self._anim = Animator(self)
        self._draw()

    @property
    def current(self):
        return self._current

    @current.setter
    def current(self, val):
        old = self._current
        self._current = max(0, min(val, self._total))
        self._animate_hp(old, self._current)

    @property
    def total(self):
        return self._total

    @total.setter
    def total(self, val):
        self._total = max(1, val)
        self._draw()

    @property
    def level(self):
        return self._level

    @level.setter
    def level(self, val):
        self._level = val
        self._draw()

    def _animate_hp(self, old_val, new_val):
        def update(t):
            self._display_current = int(lerp(old_val, new_val, t))
            self._draw()
        self._anim.animate('hp', 1000, update)

    def _draw(self):
        self.delete('all')
        w, h = self._hp_w, self._hp_h
        percent = self._display_current / max(1, self._total)

        bg_color = '#9db5d0'

        # 左侧标识方块 (22px)
        self.create_rectangle(0, 0, 22, h, fill=bg_color, outline='')
        self.create_rectangle(0, h * 0.25, 11, h * 0.75,
                              fill=self.cget('bg'), outline='')

        # 用户名区域
        self.create_rectangle(25, 0, 120, h, fill=bg_color, outline='')
        self.create_text(72, h // 2, text=self.username,
                         fill='#e1dede', font=_sao_font(9))

        # HP 条区域
        bar_x = 125
        bar_w = w - bar_x - 5
        bar_h = 23
        bar_y = (h - bar_h) // 2

        # 边框 polygon
        pts = [bar_x, bar_y,
               bar_x + bar_w, bar_y,
               bar_x + bar_w - 5, bar_y + 16,
               bar_x + bar_w * 0.45 + 4, bar_y + 16,
               bar_x + bar_w * 0.45, bar_y + bar_h,
               bar_x, bar_y + bar_h]
        self.create_polygon(pts, outline='#dad7d7', fill='', width=1)

        # HP 填充
        fill_w = int(bar_w * percent * 0.95)
        if fill_w > 0:
            if percent > 0.5:
                fill_color = '#9ad334'
            elif percent > 0.25:
                fill_color = '#f4fa49'
            else:
                fill_color = '#ef684e'
            self.create_rectangle(bar_x + 2, bar_y + 1,
                                  bar_x + 2 + fill_w, bar_y + bar_h - 1,
                                  fill=fill_color, outline='')

        # 数值
        self.create_text(bar_x + bar_w * 0.6, h - 2,
                         text=f'{self._display_current}/{self._total}',
                         fill='#e1dede', font=_sao_font(7), anchor='s')
        self.create_text(bar_x + bar_w * 0.85, h - 2,
                         text=f'Lv.{self._level}',
                         fill='#e1dede', font=_sao_font(7), anchor='s')


