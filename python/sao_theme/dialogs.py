# -*- coding: utf-8 -*-
# SAODialog / SAOLeaderboardDialog / _clip_reveal / _close_alert
# (split from sao_theme.py — verbatim).
import tkinter as tk
import time
import math
import ctypes
from typing import Optional, List, Dict
from utils.sao_sound import get_sao_font as _sao_font, get_cjk_font as _cjk_font
from sao_theme.colors import SAOColors
from sao_theme.animator import Animator
from sao_theme.utils import ease_out, lerp, _make_aa_icon_button
from gui_modules.sao_panel_ui import _remember_sao_panel_root, _theme_color

# Mirror z for dialogs registered on the unified overlay compositor — must
# sit above the default panel z (500, see render/tk_mirror.py's
# SaoToplevel(mirror_z=500) default) so an alert opened from a panel always
# renders/receives input above that panel instead of fighting over OS
# topmost order.
_SAO_DIALOG_MIRROR_Z = 2000
_sao_dialog_counter = 0

# ──────────────────── SAO 对话框 (Alert) ────────────────────
class SAODialog:
    #
    #     SAO Utils 风格对话框
    #     - 三段式: 标题区(68px) + 内容区 + 按钮区(83px)
    #     - 宽度展开动画 (135px → 375px, 0.5s)
    #     - 文字 clip 渐现
    #     - Close: 红圆 rgb(209,61,79)
    #     - OK: 蓝圆 rgb(66,140,230)
    #

    @staticmethod
    def showinfo(parent, title, message, on_ok=None):
        return SAODialog._show(parent, title, message, show_icon=True, on_ok=on_ok)

    @staticmethod
    def showwarning(parent, title, message, on_ok=None):
        return SAODialog._show(parent, title, message, show_icon=True, on_ok=on_ok)

    @staticmethod
    def showerror(parent, title, message, on_ok=None):
        return SAODialog._show(parent, title, message, show_icon=True, on_ok=on_ok)

    @staticmethod
    def ask(parent, title, message, on_ok=None, on_cancel=None):
        return SAODialog._show(parent, title, message, show_icon=True,
                        on_ok=on_ok, on_cancel=on_cancel)

    @staticmethod
    def _show(parent, title, message, show_icon=True,
              on_ok=None, on_cancel=None):
        # A plain tk.Toplevel here competes for the OS-level "always on top"
        # z-band directly against the compositor host (which re-asserts its
        # own topmost status every couple seconds — see
        # render/overlay_compositor.py's _enforce_z_order). Whichever one
        # last won that band wins the front, so a plugin-manager alert like
        # this could end up visually buried under its own parent panel.
        # Registering it as a SaoToplevel instead makes it a compositor
        # layer with an explicit, always-higher z — no OS topmost fight.
        global _sao_dialog_counter
        _sao_dialog_counter += 1
        dlg = None
        try:
            from render.tk_mirror import SaoToplevel
            dlg = SaoToplevel(
                parent, mirror_name=f'sao_dialog_{_sao_dialog_counter}',
                mirror_z=_SAO_DIALOG_MIRROR_Z)
        except Exception:
            dlg = None
        if dlg is None:
            dlg = tk.Toplevel(parent)
        _remember_sao_panel_root(dlg)
        dlg.overrideredirect(True)
        try:
            dlg.attributes('-topmost', True)
        except Exception:
            pass

        try:
            dlg.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(dlg.winfo_id())
            val = ctypes.c_int(2)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(val), 4)
        except Exception:
            pass

        final_w = 375
        final_h = 240
        initial_w = 135

        px = (dlg.winfo_screenwidth() - final_w) // 2
        py = (dlg.winfo_screenheight() - final_h) // 2

        dlg.geometry(f'{initial_w}x{final_h}+{px + (final_w - initial_w) // 2}+{py}')

        outer_bg = _theme_color('bg', '#e0e0e0')
        surface_bg = _theme_color('card_bg', '#ffffff')
        content_bg = _theme_color('body_bg', '#eae9e9')
        border = _theme_color('sep', '#e0e0e0')
        title_fg = _theme_color('value_fg', SAOColors.ALERT_TITLE_FG)
        content_fg = _theme_color('label_fg', '#888888')
        dlg.configure(bg=outer_bg)

        main_box = tk.Frame(dlg, bg=surface_bg)
        main_box.pack(fill=tk.BOTH, expand=True)
        main_box.pack_forget()

        # 标题区 (68px)
        header = tk.Frame(main_box, bg=surface_bg, height=68)
        header.pack(fill=tk.X)
        header.pack_propagate(False)

        title_lbl = tk.Label(header, text='', bg=surface_bg,
                     fg=title_fg,
                             font=_sao_font(13, True))
        title_lbl.pack(expand=True)

        tk.Frame(main_box, bg=border, height=1).pack(fill=tk.X)

        # 内容区 (浅灰)
        content_h = final_h - 68 - 83 - 2
        content = tk.Frame(main_box, bg=content_bg, height=max(25, content_h))
        content.pack(fill=tk.X)
        content.pack_propagate(False)

        content_lbl = tk.Label(content, text='', bg=content_bg,
                       fg=content_fg,
                               font=_cjk_font(10),
                               wraplength=final_w - 48, justify='center')
        content_lbl.pack(expand=True)

        tk.Frame(main_box, bg=border, height=1).pack(fill=tk.X)

        # 按钮区 (83px)
        footer = tk.Frame(main_box, bg=surface_bg, height=83)
        footer.pack(fill=tk.X)
        footer.pack_propagate(False)

        btn_container = tk.Frame(footer, bg=surface_bg)
        btn_container.place(relx=0.5, rely=0.5, anchor='center')

        def do_close():
            _close_alert(dlg)
            if on_cancel:
                on_cancel()

        def do_ok():
            _close_alert(dlg)
            if on_ok:
                on_ok()

        if show_icon:
            ok_btn = _make_aa_icon_button(btn_container, 'ok', do_ok,
                                          SAOColors.OK_BLUE, SAOColors.OK_BLUE, bg=surface_bg)
            ok_btn.pack(side=tk.LEFT, padx=20)

            close_btn = _make_aa_icon_button(btn_container, 'close', do_close,
                                             SAOColors.CLOSE_RED, SAOColors.CLOSE_RED, bg=surface_bg)
            close_btn.pack(side=tk.LEFT, padx=20)
        else:
            dlg.bind('<Button-1>', lambda e: do_close())

        # SaoToplevel only attaches its compositor mirror (and starts
        # pushing captured frames) once deiconified — do this now, right
        # before the reveal animation starts, so the whole grow/reveal
        # sequence is actually visible instead of only the final frame.
        try:
            dlg.deiconify()
        except Exception:
            pass

        # 展开动画
        anim = Animator(dlg)

        def expand(t):
            if not dlg.winfo_exists():
                return
            w = int(lerp(initial_w, final_w, t))
            x = px + (final_w - w) // 2
            dlg.geometry(f'{w}x{final_h}+{x}+{py}')

        def reveal_text():
            if not main_box.winfo_manager():
                main_box.pack(fill=tk.BOTH, expand=True)
                dlg.update_idletasks()
            _clip_reveal(title_lbl, title, dlg, 400, delay=100)
            _clip_reveal(content_lbl, message, dlg, 350, delay=600)

        anim.animate('expand', 500, expand, on_done=reveal_text)

        # 拖拽
        _drag = {'x': 0, 'y': 0}
        def start_drag(e):
            _drag['x'], _drag['y'] = e.x_root, e.y_root
        def do_drag(e):
            dx = e.x_root - _drag['x']
            dy = e.y_root - _drag['y']
            dlg.geometry(f'+{dlg.winfo_x() + dx}+{dlg.winfo_y() + dy}')
            _drag['x'], _drag['y'] = e.x_root, e.y_root
        for w in [header, title_lbl]:
            w.bind('<Button-1>', start_drag)
            w.bind('<B1-Motion>', do_drag)

        # 非阻塞: 不调用 wait_window / grab_set — 纯回调驱动
        # overrideredirect Toplevel 的 grab_set 在 Windows 上经常静默失败
        # 并导致 wait_window 永久挂起 (假性卡死)
        dlg.focus_force()
        return dlg


def _clip_reveal(label: tk.Label, full_text: str, dlg: tk.Toplevel,
                 duration_ms: int, delay: int = 0):
    # 模拟 CSS clip-path inset 渐现: 从中间向两边展开
    if not full_text:
        label.configure(text='')
        return

    def start():
        if not dlg.winfo_exists():
            return
        steps = max(1, duration_ms // 30)
        step = [0]

        def tick():
            if not dlg.winfo_exists():
                return
            t = min(step[0] / steps, 1.0)
            n = len(full_text)
            visible = int(n * t)
            s = (n - visible) // 2
            e = s + visible
            display = ' ' * s + full_text[s:e] + ' ' * (n - e)
            label.configure(text=display)
            step[0] += 1
            if t < 1.0:
                dlg.after(30, tick)
            else:
                label.configure(text=full_text)

        tick()

    if delay > 0:
        dlg.after(delay, start)
    else:
        start()


def _close_alert(dlg: tk.Toplevel):
    # 关闭对话框: 宽度收缩 → 消失
    if not dlg.winfo_exists():
        return

    # 先释放 grab
    try:
        dlg.grab_release()
    except Exception:
        pass

    cur_w = dlg.winfo_width()
    cur_h = dlg.winfo_height()
    cur_x = dlg.winfo_x()
    cur_y = dlg.winfo_y()

    anim = Animator(dlg)

    mirror = getattr(dlg, '_mirror', None)

    def shrink(t):
        if not dlg.winfo_exists():
            return
        w = max(1, int(lerp(cur_w, 0, t)))
        x = cur_x + (cur_w - w) // 2
        try:
            dlg.geometry(f'{w}x{cur_h}+{x}+{cur_y}')
            if mirror is not None:
                mirror.set_alpha(1.0 - t)
            else:
                dlg.attributes('-alpha', 1.0 - t)
        except Exception:
            pass

    def finish():
        try:
            if dlg.winfo_exists():
                dlg.destroy()
        except Exception:
            pass

    anim.animate('close', 350, shrink, on_done=finish)


class SAOLeaderboardDialog:
    # SAO 风格排行榜对话框：分页、搜索、自适应高度、显示自身设备名与排名。

    def __init__(self, parent, title='排行榜', sort_by='xp'):
        self._parent = parent
        self._title = title
        self._sort_by = sort_by
        self._entries: List[Dict] = []
        self._filtered: List[Dict] = []
        self._page = 0
        self._per_page = 10
        self._self_device = ''
        self._self_device_name = ''
        self._self_rank = None
        self._focus_rank = None

        global _sao_dialog_counter
        _sao_dialog_counter += 1
        dlg = None
        try:
            from render.tk_mirror import SaoToplevel
            dlg = SaoToplevel(
                parent, mirror_name=f'sao_leaderboard_{_sao_dialog_counter}',
                mirror_z=_SAO_DIALOG_MIRROR_Z)
        except Exception:
            dlg = None
        self._dlg = dlg if dlg is not None else tk.Toplevel(parent)
        _remember_sao_panel_root(self._dlg)
        self._dlg.overrideredirect(True)
        try:
            self._dlg.attributes('-topmost', True)
        except Exception:
            pass
        outer_bg = _theme_color('bg', '#d9dde3')
        surface_bg = _theme_color('card_bg', '#ffffff')
        toolbar_bg = _theme_color('card_bg_alt', '#f4f5f7')
        body_bg = _theme_color('body_bg', '#ececec')
        border = _theme_color('border', '#d1d7df')
        sep = _theme_color('sep', '#dde3ea')
        label_fg = _theme_color('label_fg', '#6b7888')
        value_fg = _theme_color('value_fg', '#333333')
        accent = _theme_color('gold', '#f3af12')
        accent_text = _theme_color('accent_strong', '#428ce6')
        control_bg = _theme_color('control_bg', '#ffffff')
        self._dlg.configure(bg=outer_bg)

        try:
            self._dlg.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(self._dlg.winfo_id())
            val = ctypes.c_int(2)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(val), 4)
        except Exception:
            pass

        self._final_w = 660
        self._min_h = 300
        self._max_h = 600
        self._initial_w = 180
        self._current_h = self._min_h
        self._px = (self._dlg.winfo_screenwidth() - self._final_w) // 2
        self._py = (self._dlg.winfo_screenheight() - self._current_h) // 2
        self._dlg.geometry(f'{self._initial_w}x{self._current_h}+{self._px + (self._final_w - self._initial_w)//2}+{self._py}')

        main = tk.Frame(self._dlg, bg=surface_bg)
        main.pack(fill=tk.BOTH, expand=True)

        header = tk.Frame(main, bg=surface_bg, height=58)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        tk.Frame(header, bg=accent, height=3).pack(fill=tk.X)
        self._title_lbl = tk.Label(header, text=title, bg=surface_bg, fg=value_fg, font=_sao_font(13, True))
        self._title_lbl.pack(expand=True)

        toolbar = tk.Frame(main, bg=toolbar_bg, height=54)
        toolbar.pack(fill=tk.X)
        toolbar.pack_propagate(False)
        search_wrap = tk.Frame(toolbar, bg=border)
        search_wrap.pack(side=tk.LEFT, padx=(14, 8), pady=10, fill=tk.X, expand=True)
        self._search_var = tk.StringVar()
        self._search_entry = tk.Entry(search_wrap, textvariable=self._search_var,
                                      relief='flat', bd=0, bg=control_bg, fg=value_fg,
                                      font=_cjk_font(9), insertbackground=accent)
        self._search_entry.pack(fill=tk.X, padx=2, pady=2, ipady=5)
        self._search_entry.bind('<Return>', lambda e: self._apply_search())
        search_btn = tk.Label(toolbar, text='搜索', bg=_theme_color('header_bg', '#1a2030'), fg=_theme_color('header_fg', '#e8f4f8'), font=_cjk_font(8, True),
                              padx=10, pady=5, cursor='hand2')
        search_btn.pack(side=tk.LEFT, padx=(0, 14), pady=10)
        search_btn.bind('<Button-1>', lambda e: self._apply_search())
        self._mine_btn = tk.Label(toolbar, text='我的排名', bg=_theme_color('header_bg', '#273244'), fg=_theme_color('header_fg', '#f5f8fb'), font=_cjk_font(8, True),
                      padx=10, pady=5, cursor='hand2')
        self._mine_btn.pack(side=tk.LEFT, padx=(0, 14), pady=10)
        self._mine_btn.bind('<Button-1>', lambda e: self._jump_to_self())

        self._info_bar = tk.Frame(main, bg=toolbar_bg, height=34)
        self._info_bar.pack(fill=tk.X)
        self._info_bar.pack_propagate(False)
        self._self_lbl = tk.Label(self._info_bar, text='PLAYER ID: --', bg=toolbar_bg, fg=label_fg, font=_sao_font(8))
        self._self_lbl.pack(side=tk.LEFT, padx=14)
        self._rank_lbl = tk.Label(self._info_bar, text='SELF RANK: --', bg=toolbar_bg, fg=accent, font=_sao_font(8, True))
        self._rank_lbl.pack(side=tk.RIGHT, padx=14)

        list_host = tk.Frame(main, bg=body_bg)
        list_host.pack(fill=tk.BOTH, expand=True)
        self._list_wrap = tk.Frame(list_host, bg=body_bg)
        self._list_wrap.pack(fill=tk.BOTH, expand=True, padx=12, pady=10)

        head = tk.Frame(self._list_wrap, bg=sep, height=28)
        head.pack(fill=tk.X)
        head.pack_propagate(False)
        for text, width, anchor in [('RANK', 8, 'w'), ('NAME', 22, 'w'), ('LV', 7, 'center'), ('STAT', 12, 'e')]:
            tk.Label(head, text=text, bg=sep, fg=label_fg, font=_sao_font(8), width=width, anchor=anchor).pack(side=tk.LEFT, padx=(6, 0))

        self._rows_host = tk.Frame(self._list_wrap, bg=body_bg)
        self._rows_host.pack(fill=tk.BOTH, expand=True)

        footer = tk.Frame(main, bg=surface_bg, height=68)
        footer.pack(fill=tk.X)
        footer.pack_propagate(False)
        pager = tk.Frame(footer, bg=surface_bg)
        pager.place(relx=0.5, rely=0.5, anchor='center')
        self._prev_btn = tk.Label(pager, text='PREV', bg=_theme_color('header_bg', '#1a2030'), fg=_theme_color('header_fg', '#e8f4f8'), font=_sao_font(8), padx=10, pady=5, cursor='hand2')
        self._prev_btn.pack(side=tk.LEFT, padx=8)
        self._prev_btn.bind('<Button-1>', lambda e: self._change_page(-1))
        self._page_lbl = tk.Label(pager, text='1 / 1', bg=surface_bg, fg=value_fg, font=_sao_font(9, True), width=10)
        self._page_lbl.pack(side=tk.LEFT, padx=8)
        self._next_btn = tk.Label(pager, text='NEXT', bg=_theme_color('header_bg', '#1a2030'), fg=_theme_color('header_fg', '#e8f4f8'), font=_sao_font(8), padx=10, pady=5, cursor='hand2')
        self._next_btn.pack(side=tk.LEFT, padx=8)
        self._next_btn.bind('<Button-1>', lambda e: self._change_page(1))
        close_btn = tk.Label(footer, text='CLOSE', bg=_theme_color('danger', '#d13d4f'), fg=_theme_color('active_fg', '#ffffff'), font=_sao_font(8, True), padx=10, pady=5, cursor='hand2')
        close_btn.place(relx=0.94, rely=0.5, anchor='center')
        close_btn.bind('<Button-1>', lambda e: self.close())

        self._drag = {'x': 0, 'y': 0}
        for w in (header, self._title_lbl):
            w.bind('<Button-1>', self._start_drag)
            w.bind('<B1-Motion>', self._do_drag)

        # See SAODialog._show — SaoToplevel only attaches its compositor
        # mirror (and starts pushing captured frames) once deiconified.
        try:
            self._dlg.deiconify()
        except Exception:
            pass

        self._animate_expand()
        self.set_loading('加载中...')
        try:
            self._dlg.focus_force()
        except Exception:
            pass

    def _start_drag(self, e):
        self._drag['x'], self._drag['y'] = e.x_root, e.y_root

    def _do_drag(self, e):
        dx = e.x_root - self._drag['x']
        dy = e.y_root - self._drag['y']
        self._dlg.geometry(f'+{self._dlg.winfo_x() + dx}+{self._dlg.winfo_y() + dy}')
        self._drag['x'], self._drag['y'] = e.x_root, e.y_root

    def _animate_expand(self):
        t0 = time.time()
        dur = 0.35

        def _step():
            if not self._dlg.winfo_exists():
                return
            t = min(1.0, (time.time() - t0) / dur)
            et = ease_out(t)
            w = int(lerp(self._initial_w, self._final_w, et))
            x = self._px + (self._final_w - w) // 2
            self._dlg.geometry(f'{w}x{self._current_h}+{x}+{self._py}')
            if t < 1.0:
                self._dlg.after(16, _step)
        _step()

    def close(self):
        _close_alert(self._dlg)

    def set_loading(self, message='加载中...'):
        self._entries = []
        self._filtered = []
        self._render_rows(message=message, empty=True)

    def set_error(self, message: str):
        self._render_rows(message=message, empty=True)

    def set_entries(self, entries: List[Dict], self_device: str, self_device_name: str = '', sort_by: str = 'xp'):
        self._entries = list(entries or [])
        self._self_device = self_device or ''
        self._self_device_name = self_device_name or ''
        self._sort_by = sort_by
        self._self_rank = None
        self._focus_rank = None
        for i, row in enumerate(self._entries):
            row.setdefault('rank', i + 1)
            if row.get('device_id', '') == self_device:
                self._self_rank = row.get('rank', i + 1)
                self._self_device_name = self._self_device_name or str(row.get('player_id', '') or row.get('username', '')).strip()
        self._apply_search()

    def _stat_text(self, row: Dict) -> str:
        if self._sort_by == 'level':
            return f"LV {row.get('level', 1)}"
        if self._sort_by == 'songs_played':
            return f"{row.get('songs_played', 0)}曲"
        if self._sort_by == 'play_time':
            sec = float(row.get('play_time', 0) or 0)
            if sec < 60:
                return f'{int(sec)}S'
            if sec < 3600:
                return f'{int(sec // 60)}M'
            return f'{sec / 3600:.1f}H'
        return f"XP {row.get('xp', row.get('total_xp', 0))}"

    def _apply_search(self):
        q = (self._search_var.get() or '').strip().lower()
        self._focus_rank = None
        if not q:
            self._filtered = list(self._entries)
        elif (q.startswith('#') and q[1:].isdigit()) or q.isdigit():
            target_rank = int(q[1:] if q.startswith('#') else q)
            self._filtered = list(self._entries)
            idx = next((i for i, row in enumerate(self._filtered) if int(row.get('rank', -1)) == target_rank), -1)
            if idx >= 0:
                self._focus_rank = target_rank
                self._page = idx // self._per_page
                self._refresh()
                return
            self._filtered = []
        else:
            self._filtered = []
            for row in self._entries:
                hay = ' '.join([
                    str(row.get('rank', '')),
                    str(row.get('player_id', '')),
                    str(row.get('username', '')),
                    str(row.get('profession', '')),
                    str(row.get('device_id', '')),
                ]).lower()
                if q in hay:
                    self._filtered.append(row)
        self._page = 0
        self._refresh()

    def _jump_to_self(self):
        if not self._self_rank:
            return
        self._search_var.set(f'#{self._self_rank}')
        self._apply_search()

    def _change_page(self, delta: int):
        pages = max(1, math.ceil(len(self._filtered) / self._per_page))
        self._page = max(0, min(pages - 1, self._page + delta))
        self._refresh()

    def _refresh(self):
        if self._self_device_name or self._self_device:
            shown = self._self_device_name or 'Player'
            self._self_lbl.configure(text=f'PLAYER ID: {shown}')
        else:
            self._self_lbl.configure(text='PLAYER ID: --')
        if self._self_rank:
            self._rank_lbl.configure(text=f'SELF RANK: #{self._self_rank}')
        else:
            self._rank_lbl.configure(text='SELF RANK: --')

        if not self._filtered:
            self._render_rows(message='未找到匹配的排名记录', empty=True)
            self._page_lbl.configure(text='0 / 0')
            return

        pages = max(1, math.ceil(len(self._filtered) / self._per_page))
        self._page = max(0, min(pages - 1, self._page))
        start = self._page * self._per_page
        page_rows = self._filtered[start:start + self._per_page]
        self._page_lbl.configure(text=f'{self._page + 1} / {pages}')
        control_bg = _theme_color('header_bg', '#1a2030')
        disabled_bg = _theme_color('border', '#9aa4b3')
        self._prev_btn.configure(bg=control_bg if self._page > 0 else disabled_bg)
        self._next_btn.configure(bg=control_bg if self._page < pages - 1 else disabled_bg)
        self._render_rows(rows=page_rows)

    def _render_rows(self, rows: Optional[List[Dict]] = None, message: str = '', empty: bool = False):
        for w in self._rows_host.winfo_children():
            w.destroy()

        visible_rows = 1
        if empty:
            tk.Label(self._rows_host, text=message,
                     bg=_theme_color('body_bg', '#ececec'),
                     fg=_theme_color('label_fg', '#8892a0'),
                     font=_cjk_font(10), pady=28).pack(fill=tk.BOTH, expand=True)
        else:
            rows = rows or []
            visible_rows = max(1, min(self._per_page, len(rows)))
            for idx, row in enumerate(rows):
                bg = (_theme_color('card_bg', '#f7f9fb') if idx % 2 == 0
                      else _theme_color('card_bg_alt', '#eef2f6'))
                if row.get('device_id', '') == self._self_device:
                    bg = _theme_color('accent_soft', '#e7f1fb')
                if self._focus_rank and int(row.get('rank', -1)) == int(self._focus_rank):
                    bg = _theme_color('gold_soft', '#fff1d8')
                line = tk.Frame(self._rows_host, bg=bg, height=34)
                line.pack(fill=tk.X, pady=1)
                line.pack_propagate(False)
                rank_text = f"#{row.get('rank', idx + 1)}"
                if row.get('rank', 99) <= 3:
                    rank_text = ['TOP1', 'TOP2', 'TOP3'][row.get('rank', 1) - 1]
                tk.Label(line, text=rank_text, bg=bg,
                         fg=(_theme_color('gold', '#f3af12') if row.get('rank', 9) <= 3 else _theme_color('label_fg', '#7a8796')),
                         font=_sao_font(8, True), width=8, anchor='w').pack(side=tk.LEFT, padx=(8, 0))
                primary = str(row.get('player_id', '') or row.get('username', '???'))[:18]
                alt = str(row.get('username', '') or '').strip()
                prof = row.get('profession', '')
                pieces = [primary]
                if alt and alt != primary:
                    pieces.append(f'@{alt[:10]}')
                if prof:
                    pieces.append(f'[{prof}]')
                tk.Label(line, text='  '.join(pieces), bg=bg, fg=_theme_color('value_fg', '#333333'),
                         font=_cjk_font(9), anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True)
                tk.Label(line, text=f"Lv.{row.get('level', 1)}", bg=bg, fg=_theme_color('accent_strong', '#428ce6'),
                         font=_sao_font(8), width=7, anchor='center').pack(side=tk.LEFT)
                tk.Label(line, text=self._stat_text(row), bg=bg, fg=_theme_color('label_fg', '#666666'),
                         font=_sao_font(8), width=12, anchor='e').pack(side=tk.RIGHT, padx=(0, 8))

        target_h = 58 + 54 + 34 + 10 + 28 + visible_rows * 36 + 68
        self._current_h = max(self._min_h, min(self._max_h, target_h))
        self._py = (self._dlg.winfo_screenheight() - self._current_h) // 2
        try:
            self._dlg.geometry(f'{self._final_w}x{self._current_h}+{self._px}+{self._py}')
        except Exception:
            pass


