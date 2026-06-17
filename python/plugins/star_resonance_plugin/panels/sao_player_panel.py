# -*- coding: utf-8 -*-
"""
SAOPlayerPanel — Star Resonance menu's top-left info panel
(username + level/exp + HP/STA).

Moved out of gui_modules so game-specific Entity/Tk menu UI stays inside
the Star Resonance plugin. Public surface remains:
``SAOPlayerPanel(parent, username, profession, panel_width=None, **kw)``
plus ``update_level()`` and animation hooks.
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Optional, Tuple

import _sao_cy_sr_uihelpers as _CY_UI  # type: ignore[import-not-found]
from utils.sao_sound import get_sao_font, get_cjk_font
# Animator imported lazily inside __init__ to break the
# sao_theme → ui_gpu.popup → sao_player_panel circular import.


class SAOPlayerPanel(tk.Frame):
    """
    SAO 风格左侧信息面板 — 对标 SAO-UI LeftInfo + HP 组件

    结构:
    - Top 区 (白色, 240×280): 用户名/分隔线/等级/EXP/HP/STA
    - Bottom 区 (灰色, 240×120): 菜单模式状态
    - 右三角指示器 (连接 MenuBar)
    - 下三角装饰 (连接 top/bottom)
    """

    ENTITY_GPU_ONLY = True

    def __init__(self, parent, username='Player', profession='',
                 panel_width: int | None = None, **kw):
        # GPU mode flag — when on, the visible pixels come from
        # PlayerPanelGpuPainter. The Tk widgets stay sized to the
        # final target with bg=chroma (#010101) so the parent shell's
        # ``-transparentcolor`` makes them invisible.
        try:
            from .sao_left_info_gpu import (
                PlayerPanelGpuPainter as _PPGP,
                _PlayerPanelSnapshot as _PPSnap,
                gpu_player_panel_enabled as _gppen,
            )
        except Exception as _player_gpu_import_error:
            _PLAYER_GPU_IMPORT_ERROR = _player_gpu_import_error
            _PPGP = None
            _PPSnap = None
            def _gppen():  # type: ignore[no-redef]
                raise RuntimeError('PlayerPanelGpuPainter GPU module is required') from _PLAYER_GPU_IMPORT_ERROR
        self._PPGP_cls = _PPGP
        self._PPSnap_cls = _PPSnap
        self._gpu_required = bool(self.ENTITY_GPU_ONLY)
        if self._gpu_required and _PPGP is None:
            _gppen()
        self._gpu_managed = bool(_PPGP is not None and _gppen())
        self._gpu_chroma = '#010101'
        self._gpu_painter = None
        # Animated geometry stash (snapshot fields). Tk widget sizes
        # stay at the final target.
        self._anim_top_w = 0
        self._anim_top_h = 0
        self._anim_bot_w = 0
        self._anim_bot_h = 0
        # winfo_rootx/y caching (Phase A pattern from
        # v2_3_0_phase2_gpu_overlay).
        self._cached_screen_xy: Optional[Tuple[int, int]] = None

        bg_color = (
            self._gpu_chroma
            if (self._gpu_managed or self._gpu_required) else '#ffffff'
        )
        super().__init__(parent, bg=bg_color, highlightthickness=0, **kw)
        self._active = False
        from sao_theme import Animator  # lazy: avoids circular import
        self._anim = Animator(self)
        try:
            self._target_w = max(120, int(panel_width or 240))
        except Exception:
            self._target_w = 240
        self._top_h = 240
        self._bottom_h = 80

        # 用户资料
        self._username = username
        self._profession = profession

        # 角色 / 赛季进度
        self._level = 1
        self._level_extra = 0
        self._season_exp = 0
        self._sta_hp = (0, 0)
        self._sta_sta = (0, 0)
        self._shift_mode = "普通模式"
        self._on_mode_change = None  # callback(mode_text) 供外部持久化

        self._build()
        if self._gpu_managed:
            self._setup_gpu_painter()

    def _build(self):
        # In GPU mode the canvases are sized ONCE to the final target
        # so the parent shell reserves the right area; their bg is
        # the chroma key so they are invisible. The animated visuals
        # live entirely in the GPU painter. In CPU mode we keep the
        # legacy 0×0-then-grow behaviour.
        if self._gpu_managed or self._gpu_required:
            top_w = self._target_w
            top_h = self._top_h
            bot_w = self._target_w
            bot_h = self._bottom_h
            self._top = tk.Canvas(self, width=top_w, height=top_h,
                                  bg=self._gpu_chroma, highlightthickness=0)
            self._top.pack(anchor='nw')
            self._bottom = tk.Canvas(self, width=bot_w, height=bot_h,
                                     bg=self._gpu_chroma, highlightthickness=0)
            self._bottom.pack(anchor='nw')
        else:
            self._top = tk.Canvas(self, width=0, height=0,
                                  bg='#ffffff', highlightthickness=0)
            self._top.pack(anchor='nw')
            self._bottom = tk.Canvas(self, width=0, height=0,
                                     bg='#e5e3e3', highlightthickness=0)
            self._bottom.pack(anchor='nw')

    def set_active(self, active: bool):
        if active == self._active:
            return
        self._active = active
        if active:
            self._animate_open()
        else:
            self._animate_close()

    def update_shift_mode(self, mode_text: str):
        self._shift_mode = mode_text
        if self._active:
            if self._gpu_managed:
                self._dispatch_gpu_paint()
            elif not self._gpu_required:
                self._redraw_bottom(self._target_w, self._bottom_h)
        if self._on_mode_change:
            try:
                self._on_mode_change(mode_text)
            except Exception:
                pass

    def _repaint_top_if_active(self) -> None:
        if not self._active:
            return
        if self._gpu_managed:
            self._dispatch_gpu_paint()
        elif not self._gpu_required:
            self._redraw_top(self._target_w, self._top_h)

    @staticmethod
    def _coerce_stat_pair(value, default=(0, 0)) -> tuple[int, int]:
        try:
            cur, max_value = value
        except Exception:
            return default
        try:
            return max(0, int(cur or 0)), max(0, int(max_value or 0))
        except Exception:
            return default

    def update_profile(self, username=None, profession=None,
                       repaint: bool = True) -> bool:
        """Update displayed player identity and repaint the top plate."""
        next_username = self._username if username is None else str(username)
        next_profession = self._profession if profession is None else str(profession)
        if next_username == self._username and next_profession == self._profession:
            return False
        self._username = next_username
        self._profession = next_profession
        if repaint:
            self._repaint_top_if_active()
        return True

    def update_vitals(self, hp=None, sta=None, repaint: bool = True) -> bool:
        """Update HP/STA values shown in the player panel."""
        cur_hp = self._coerce_stat_pair(getattr(self, '_sta_hp', (0, 0)))
        cur_sta = self._coerce_stat_pair(getattr(self, '_sta_sta', (0, 0)))
        next_hp = cur_hp if hp is None else self._coerce_stat_pair(hp, cur_hp)
        next_sta = cur_sta if sta is None else self._coerce_stat_pair(sta, cur_sta)
        if next_hp == cur_hp and next_sta == cur_sta:
            return False
        self._sta_hp = next_hp
        self._sta_sta = next_sta
        if repaint:
            self._repaint_top_if_active()
        return True

    def update_level(self, level: int, level_extra: int = 0,
                     season_exp: Optional[int] = None):
        """更新等级/赛季等级/EXP 信息。"""
        level = max(1, int(level or 1))
        level_extra = max(0, int(level_extra or 0))
        next_exp = self._season_exp if season_exp is None else max(0, int(season_exp or 0))
        if (
            self._level == level and
            self._level_extra == level_extra and
            self._season_exp == next_exp
        ):
            return
        self._level = level
        self._level_extra = level_extra
        self._season_exp = next_exp
        self._repaint_top_if_active()

    def _animate_open(self):
        if self._gpu_managed:
            def phase1(t):
                w, h = _CY_UI.player_panel_anim_size(self._target_w, self._top_h, t)
                self._anim_top_w = w
                self._anim_top_h = h
                self._dispatch_gpu_paint()

            def phase2(t):
                _, h = _CY_UI.player_panel_anim_size(self._target_w, self._bottom_h, t)
                self._anim_bot_w = self._target_w
                self._anim_bot_h = h
                self._dispatch_gpu_paint()

            self._anim.animate('top_open', 500, phase1,
                               on_done=lambda: self._anim.animate('bottom_open', 400, phase2))
            return
        if self._gpu_required:
            return

        def phase1(t):
            w, h = _CY_UI.player_panel_anim_size(self._target_w, self._top_h, t)
            self._top.configure(width=w, height=h)
            self._redraw_top(w, h)

        def phase2(t):
            _, h = _CY_UI.player_panel_anim_size(self._target_w, self._bottom_h, t)
            self._bottom.configure(width=self._target_w, height=h)
            self._redraw_bottom(self._target_w, h)

        self._anim.animate('top_open', 500, phase1,
                           on_done=lambda: self._anim.animate('bottom_open', 400, phase2))

    def _animate_close(self):
        if self._gpu_managed:
            def fade(t):
                inv = 1 - t
                w, top_h = _CY_UI.player_panel_anim_size(self._target_w, self._top_h, inv)
                _, bot_h = _CY_UI.player_panel_anim_size(self._target_w, self._bottom_h, inv)
                self._anim_top_w = w
                self._anim_top_h = top_h
                self._anim_bot_w = w
                self._anim_bot_h = bot_h
                self._dispatch_gpu_paint()

            self._anim.animate('close', 200, fade)
            return
        if self._gpu_required:
            return

        def fade(t):
            inv = 1 - t
            w, top_h = _CY_UI.player_panel_anim_size(self._target_w, self._top_h, inv)
            _, bot_h = _CY_UI.player_panel_anim_size(self._target_w, self._bottom_h, inv)
            self._top.configure(width=w, height=top_h)
            self._bottom.configure(width=w, height=bot_h)

        self._anim.animate('close', 200, fade)

    def _redraw_top(self, w, h):
        """SAO 系统信息面板 .top — HUD 风格"""
        if self._gpu_managed:
            # External callers (e.g. sao_gui's profile pull) might call
            # this with the final target size. In GPU mode just push a
            # snapshot; never paint to Tk.
            self._anim_top_w = max(self._anim_top_w, int(w))
            self._anim_top_h = max(self._anim_top_h, int(h))
            self._dispatch_gpu_paint()
            return
        if self._gpu_required:
            return
        self._top.delete('all')
        if w < 40 or h < 40:
            return

        GOLD = '#f3af12'
        CYAN = '#86dfff'
        DIM = '#c8c8c8'
        LABEL = '#aaaaaa'
        TITLE_FG = '#646364'

        # ── 背景 ──
        self._top.create_rectangle(0, 0, w, h, fill='#ffffff', outline='')

        # ── HUD 角标 (四角 L 型边框) ──
        bk = 14  # bracket length
        self._top.create_line(2, 2, 2 + bk, 2, fill=CYAN, width=1)
        self._top.create_line(2, 2, 2, 2 + bk, fill=CYAN, width=1)
        self._top.create_line(w - 2 - bk, 2, w - 2, 2, fill=GOLD, width=1)
        self._top.create_line(w - 2, 2, w - 2, 2 + bk, fill=GOLD, width=1)
        self._top.create_line(2, h - 2, 2 + bk, h - 2, fill=CYAN, width=1)
        self._top.create_line(2, h - 2 - bk, 2, h - 2, fill=CYAN, width=1)
        self._top.create_line(w - 2 - bk, h - 2, w - 2, h - 2, fill=GOLD, width=1)
        self._top.create_line(w - 2, h - 2 - bk, w - 2, h - 2, fill=GOLD, width=1)

        # ── 右三角指示器 (连接 MenuBar) ──
        tri_y = int(h * 0.6)
        self._top.create_polygon(w, tri_y, w + 18, tri_y + 7, w, tri_y + 14,
                                 fill='#ffffff', outline='')

        # ── 系统编号标签 ──
        self._top.create_text(w - 8, 12, text='SYS:PLAYER', anchor='e',
                              font=get_sao_font(6), fill=DIM)

        # ── 用户名 ──
        title_y = 26
        display_name = self._username
        if len(display_name) > 18:
            display_name = display_name[:16] + '…'
        self._top.create_text(w // 2, title_y, text=display_name,
                              font=get_sao_font(13, True), fill=TITLE_FG)

        # 分隔线 (对标 .title border-bottom)
        sep_y = 44
        self._top.create_line(10, sep_y, w - 10, sep_y, fill='#aaaaaa', width=2)
        # 微型扫描点
        for i in range(5):
            dot_x = 14 + i * 8
            self._top.create_rectangle(dot_x, sep_y - 1, dot_x + 3, sep_y,
                                       fill=CYAN, outline='')

        if h < 60:
            return

        # ── 等级区域 ──
        # 等级标签
        self._top.create_text(20, 62, text='LEVEL', anchor='w',
                              font=get_sao_font(7), fill=LABEL)
        level_text = f'Lv. {self._level}'
        level_font_size = 20
        if self._level_extra > 0:
            level_text = f'Lv. {self._level}(+{self._level_extra})'
            level_font_size = 16 if self._level_extra < 100 else 14
        self._top.create_text(w // 2, 84,
                              text=level_text,
                              font=get_sao_font(level_font_size, True), fill=GOLD)

        if h < 120:
            return

        # ── 赛季 EXP ──
        exp_text = f'{self._season_exp:,}' if self._season_exp > 0 else '—'

        # EXP 标签
        self._top.create_text(20, 108, text='EXP', anchor='w',
                              font=get_sao_font(7), fill=LABEL)
        self._top.create_text(w - 20, 108, text=exp_text,
                              anchor='e', font=get_sao_font(8), fill='#999999')

        # EXP 条 (当前协议仅提供 CurExp, 无下一等级阈值时保留轨道)
        if h > 124:
            xp_y = 122
            xp_x = 20
            xp_w = w - 40
            xp_h = 6
            # 底色
            self._top.create_rectangle(xp_x, xp_y, xp_x + xp_w, xp_y + xp_h,
                                       fill='#e8e8e8', outline='#d8d8d8', width=1)
            if self._season_exp > 0:
                self._top.create_rectangle(xp_x + 1, xp_y + 1,
                                           xp_x + 10, xp_y + xp_h - 1,
                                           fill=GOLD, outline='')
                self._top.create_rectangle(xp_x + 1, xp_y + 1,
                                           xp_x + 10, xp_y + 3,
                                           fill='#f5c644', outline='')

        # ── 状态标签行 (使用游戏识别数据) ──
        if h > 150:
            info_y = 140
            # HP 状态 (来自识别引擎)
            hp_c, hp_m = getattr(self, '_sta_hp', (0, 0))
            self._top.create_text(20, info_y, text='HP', anchor='w',
                                  font=get_sao_font(7, True), fill=CYAN)
            hp_text = f'{hp_c}/{hp_m}' if hp_m > 0 else '—'
            self._top.create_text(w - 20, info_y, text=hp_text, anchor='e',
                                  font=get_sao_font(8), fill='#777777')

        if h > 170:
            info_y2 = 158
            # STA 状态 (来自识别引擎)
            sta_c, sta_m = getattr(self, '_sta_sta', (0, 0))
            self._top.create_text(20, info_y2, text='STA', anchor='w',
                                  font=get_sao_font(7, True), fill=GOLD)
            sta_text = f'{sta_c}/{sta_m}' if sta_m > 0 else '—'
            self._top.create_text(w - 20, info_y2, text=sta_text, anchor='e',
                                  font=get_sao_font(8), fill='#777777')

        # ── 底部微型扫描线 ──
        if h > 185:
            scan_y = h - 16
            self._top.create_line(10, scan_y, w - 10, scan_y, fill='#e8e8e8', width=1)
            scan_x = _CY_UI.scan_x(w, time.time())
            self._top.create_rectangle(scan_x - 12, scan_y - 1,
                                       scan_x + 12, scan_y + 1,
                                       fill=CYAN, outline='')

    def _redraw_bottom(self, w, h):
        """SAO 系统信息面板 .bottom — 状态描述区"""
        if self._gpu_managed:
            self._anim_bot_w = max(self._anim_bot_w, int(w))
            self._anim_bot_h = max(self._anim_bot_h, int(h))
            self._dispatch_gpu_paint()
            return
        if self._gpu_required:
            return
        self._bottom.delete('all')
        if w < 40 or h < 15:
            return

        # 背景
        self._bottom.create_rectangle(0, 0, w, h, fill='#e5e3e3', outline='')

        # 下三角装饰 (连接 top/bottom)
        self._bottom.create_polygon(30, 0, 37.5, -10, 45, 0,
                                    fill='#e5e3e3', outline='')

        # 顶部微渐变阴影
        for i in range(3):
            av = int(220 + i * 8)
            self._bottom.create_line(0, i, w, i,
                                     fill=f'#{av:02x}{av:02x}{av:02x}', width=1)

        # 角标
        self._bottom.create_line(3, 3, 12, 3, fill='#86dfff', width=1)
        self._bottom.create_line(3, 3, 3, 12, fill='#86dfff', width=1)
        self._bottom.create_line(w - 12, h - 3, w - 3, h - 3, fill='#f3af12', width=1)
        self._bottom.create_line(w - 3, h - 12, w - 3, h - 3, fill='#f3af12', width=1)

        # 状态标签
        self._bottom.create_text(12, 12, text='STATUS', anchor='w',
                                 font=get_sao_font(6), fill='#b0b0b0')

        # ── 菜单模式 ──
        sm = self._shift_mode if self._shift_mode else '普通模式'
        sm_color = '#2196f3' if sm == '普通模式' else ('#e65100' if 'CTRL' in sm else '#1565c0')
        self._bottom.create_text(15, h // 2 - 2, text=sm,
                                 font=get_cjk_font(9, True), fill=sm_color,
                                 anchor='w')

        # 底部系统标签
        if h > 50:
            self._bottom.create_text(w - 8, h - 10, text='SAO://SYSTEM',
                                     anchor='e', font=get_sao_font(5), fill='#c8c8c8')

    # ── GPU painter wiring (GPU-required Entity path) ─

    def _setup_gpu_painter(self) -> None:
        if self._PPGP_cls is None:
            raise RuntimeError('PlayerPanelGpuPainter GPU module is required')
        try:
            self._gpu_painter = self._PPGP_cls(self.winfo_toplevel())
        except Exception:
            self._gpu_painter = None
            self._gpu_managed = False
            raise
        # winfo_rootx/y is expensive; cache and invalidate on
        # <Configure> (matches Phase A pattern).
        self.bind('<Configure>',
                  lambda e: setattr(self, '_cached_screen_xy', None),
                  add='+')
        self.bind('<Destroy>', lambda e: self._on_gpu_destroy(), add='+')
        try:
            self.after(1, self._warmup_gpu_painter)
        except Exception:
            pass

    def _warmup_gpu_painter(self) -> None:
        painter = self._gpu_painter
        warmup = getattr(painter, 'warmup', None)
        if not callable(warmup):
            return
        try:
            sx, sy = self.winfo_rootx(), self.winfo_rooty()
        except Exception:
            sx, sy = 0, 0
        try:
            warmup(self._target_w, self._top_h + self._bottom_h, sx, sy)
        except Exception:
            pass

    def _on_gpu_destroy(self) -> None:
        if self._gpu_painter is not None:
            try:
                self._gpu_painter.destroy()
            except Exception:
                pass
            self._gpu_painter = None

    def _dispatch_gpu_paint(self) -> None:
        painter = self._gpu_painter
        snap_cls = self._PPSnap_cls
        if painter is None or snap_cls is None:
            return
        try:
            if not self.winfo_exists():
                return
        except Exception:
            return
        # Animated sizes: fall back to final target if not yet set
        # (e.g. external _redraw_top call before any animation tick).
        top_w = self._anim_top_w if self._anim_top_w > 0 else self._target_w
        top_h = self._anim_top_h if self._anim_top_h > 0 else self._top_h
        bot_w = self._anim_bot_w if self._anim_bot_w > 0 else self._target_w
        bot_h = self._anim_bot_h if self._anim_bot_h > 0 else self._bottom_h
        # Match Tk: scan dot uses time.time()*1.5 sin → [0,1] phase
        scan_phase = _CY_UI.scan_phase(time.time())
        snap = snap_cls(
            self._username,
            self._level, self._level_extra, self._season_exp,
            self._sta_hp, self._sta_sta,
            self._shift_mode,
            top_w, top_h, bot_w, bot_h, scan_phase,
        )
        if self._cached_screen_xy is None:
            try:
                sx = self.winfo_rootx()
                sy = self.winfo_rooty()
                self._cached_screen_xy = (sx, sy)
            except Exception:
                self._cached_screen_xy = (0, 0)
        sx, sy = self._cached_screen_xy
        try:
            painter.tick(sx, sy, snap)
        except Exception:
            pass
