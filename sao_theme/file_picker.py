# -*- coding: utf-8 -*-
"""SAOFilePicker (split from sao_theme.py — verbatim)."""
import tkinter as tk
import os
import time
import ctypes
from utils.sao_sound import get_sao_font as _sao_font, get_cjk_font as _cjk_font
from sao_theme.colors import SAOColors
from sao_theme.utils import _make_aa_icon_button
from sao_theme.dialogs import _clip_reveal

# ──────────────────── SAO 文件选择器 ────────────────────
class SAOFilePicker(tk.Toplevel):
    """SAO 风格文件浏览器 — 白色主题"""

    _BG       = '#ffffff'
    _BG2      = '#f5f5f7'
    _BORDER   = '#d1d1d6'
    _ACCENT   = '#f3af12'
    _ACCENT2  = '#dea620'
    _TEXT     = '#333333'
    _TEXT_DIM = '#999999'
    _SEL_BG   = '#fff3c0'
    _SEL_FG   = '#333333'
    _DIR_FG   = '#e67c00'
    _FILE_FG  = '#444444'

    def __init__(self, parent, title='选择文件', initial_dir='.',
                 filetypes=None, callback=None, mode='file', **kw):
        super().__init__(parent)
        self.result = None
        self.callback = callback
        self._dialog_title = title
        self._current_dir = os.path.abspath(initial_dir)
        self._filetypes = filetypes or [('All Files', '*.*')]
        self._entries = []
        self._mode = mode  # 'file' or 'dir'

        self.withdraw()
        self.overrideredirect(True)
        self.attributes('-topmost', True)
        self.attributes('-alpha', 0.0)
        self.configure(bg='#e0e0e0')

        self._final_w, self._final_h = 520, 480
        self._initial_w = 135
        # 始终居中于屏幕
        self._px = (self.winfo_screenwidth()  - self._final_w) // 2
        self._py = (self.winfo_screenheight() - self._final_h) // 2
        # 从窄条开始 (与 SAODialog 展开风格一致)
        self.geometry(f'{self._initial_w}x{self._final_h}'
                      f'+{self._px + (self._final_w - self._initial_w) // 2}'
                      f'+{self._py}')

        try:
            self.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(self.winfo_id())
            val = ctypes.c_int(2)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(val), 4)
        except Exception:
            pass

        self._build_ui(self._final_w, self._final_h, title)
        if getattr(self, '_main_box', None):
            self._main_box.pack_forget()
        self._load_dir(self._current_dir)
        self.update_idletasks()

        self._drag = {'x': 0, 'y': 0}
        self.transient(parent)
        self.deiconify()
        # 展开动画 → 完成后 grab
        self.after(50, self._animate_expand)

    def _delayed_grab(self):
        """窗口完全映射后才 grab_set, 防止 grab 冲突导致窗口闪退"""
        try:
            if self.winfo_exists():
                self.lift()
                self.grab_set()
                self.focus_force()
        except Exception:
            pass

    def _animate_expand(self):
        """SAO 风格宽度展开动画 (135px → 520px, 500ms ease-out cubic)."""
        import time as _time
        t0 = _time.time()
        dur = 0.5
        fw = self._final_w
        fh = self._final_h
        iw = self._initial_w
        px = self._px
        py = self._py

        def _step():
            if not self.winfo_exists():
                return
            elapsed = _time.time() - t0
            t = min(1.0, elapsed / dur)
            et = 1.0 - (1.0 - t) ** 3  # ease-out cubic
            w = int(iw + (fw - iw) * et)
            x = px + (fw - w) // 2
            self.geometry(f'{w}x{fh}+{x}+{py}')
            try:
                self.attributes('-alpha', min(1.0, 0.15 + t * 0.85))
            except Exception:
                pass
            if t < 1.0:
                self.after(16, _step)
            else:
                try:
                    self.attributes('-alpha', 1.0)
                except Exception:
                    pass
                if getattr(self, '_main_box', None) and not self._main_box.winfo_manager():
                    self._main_box.pack(fill=tk.BOTH, expand=True)
                    self.update_idletasks()
                if hasattr(self, '_title_lbl'):
                    _clip_reveal(self._title_lbl, self._dialog_title, self, 380, delay=40)
                # 展开完成, grab 焦点
                self.after(50, self._delayed_grab)
        _step()

    def _build_ui(self, w, h, title):
        # ── SAODialog 式三段壳 ──
        main_box = tk.Frame(self, bg='#ffffff')
        main_box.pack(fill=tk.BOTH, expand=True)
        self._main_box = main_box

        # 标题区 (68px)
        header = tk.Frame(main_box, bg='#ffffff', height=68)
        header.pack(fill=tk.X)
        header.pack_propagate(False)

        # 菱形图标
        hcv = tk.Canvas(header, width=24, height=24,
                        bg='#ffffff', highlightthickness=0)
        hcv.pack(side=tk.LEFT, padx=(16, 0), pady=22)
        hcv.create_polygon(12, 2, 22, 12, 12, 22, 2, 12,
                           fill=self._ACCENT, outline='')

        self._title_lbl = tk.Label(header, text='', bg='#ffffff',
                                   fg=SAOColors.ALERT_TITLE_FG,
                                   font=_sao_font(13, True))
        self._title_lbl.place(relx=0.5, rely=0.5, anchor='center')

        # 关闭 ×
        close_btn = _make_aa_icon_button(header, 'close', self._cancel,
                         SAOColors.CLOSE_RED, SAOColors.CLOSE_RED, bg='#ffffff')
        close_btn.pack(side=tk.RIGHT, padx=16, pady=14)

        tk.Frame(main_box, bg='#e0e0e0', height=1).pack(fill=tk.X)

        # 内容区 (浅灰)
        content = tk.Frame(main_box, bg='#eae9e9')
        content.pack(fill=tk.BOTH, expand=True)

        for w_item in [header, self._title_lbl]:
            w_item.bind('<Button-1>', self._start_drag)
            w_item.bind('<B1-Motion>', self._do_drag)

        # ── 路径行 ──
        path_row = tk.Frame(content, bg='#eae9e9', height=30)
        path_row.pack(fill=tk.X, padx=10, pady=(10, 0))
        path_row.pack_propagate(False)

        tk.Label(path_row, text='▸', bg='#eae9e9', fg=self._ACCENT2,
                 font=_sao_font(8)).pack(side=tk.LEFT, padx=(6, 4), pady=6)
        self._path_lbl = tk.Label(path_row, text='', bg=self._BG,
                                  fg=self._TEXT_DIM,
                                  font=_sao_font(8), anchor='w')
        self._path_lbl.configure(bg='#eae9e9')
        self._path_lbl.pack(side=tk.LEFT, fill=tk.X, expand=True, pady=6)

        # ── 列表区 ──
        list_outer = tk.Frame(content, bg=self._BORDER, padx=0, pady=0)
        list_outer.pack(fill=tk.BOTH, expand=True, padx=14, pady=(8, 8))

        list_frame = tk.Frame(list_outer, bg=self._BG2)
        list_frame.pack(fill=tk.BOTH, expand=True, padx=1, pady=1)

        # 自定义滚动条
        sb_frame = tk.Frame(list_frame, bg=self._BG2, width=8)
        sb_frame.pack(side=tk.RIGHT, fill=tk.Y)
        scrollbar = tk.Scrollbar(sb_frame, orient=tk.VERTICAL,
                                 troughcolor=self._BG2,
                                 bg=self._BORDER, width=8,
                                 highlightthickness=0, bd=0)
        scrollbar.pack(fill=tk.Y, expand=True)

        self._listbox = tk.Listbox(
            list_frame,
            bg=self._BG2, fg=self._FILE_FG,
            font=_cjk_font(9),
            selectbackground=self._SEL_BG,
            selectforeground=self._SEL_FG,
            highlightthickness=0, bd=0,
            activestyle='none',
            yscrollcommand=scrollbar.set,
            relief=tk.FLAT,
        )
        self._listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=self._listbox.yview)
        self._listbox.bind('<Double-Button-1>', self._on_double_click)
        self._listbox.bind('<Return>', lambda e: self._confirm())

        # ── 底部分隔线 ──
        tk.Frame(content, bg=self._BORDER, height=1).pack(fill=tk.X, padx=14)

        # ── 文件名预览行 ──
        fname_row = tk.Frame(content, bg='#eae9e9', height=30)
        fname_row.pack(fill=tk.X, padx=14, pady=(4, 10))
        fname_row.pack_propagate(False)
        self._fname_lbl = tk.Label(fname_row, text='未选择文件', bg='#eae9e9',
                                   fg=self._ACCENT, font=_cjk_font(9),
                                   anchor='w')
        self._fname_lbl.pack(fill=tk.X, padx=4, pady=4)
        self._listbox.bind('<<ListboxSelect>>', self._on_select)

        # 按钮区 (83px)
        tk.Frame(main_box, bg='#e0e0e0', height=1).pack(fill=tk.X)
        footer = tk.Frame(main_box, bg='#ffffff', height=83)
        footer.pack(fill=tk.X)
        footer.pack_propagate(False)

        btn_frame = tk.Frame(footer, bg='#ffffff')
        btn_frame.place(relx=0.5, rely=0.5, anchor='center')

        # 目录模式: 添加 "选择此文件夹" 按钮
        if self._mode == 'dir':
            sel_dir_btn = _make_aa_icon_button(btn_frame, 'ok', self._confirm_dir,
                                               '#4caf50', '#4caf50', bg='#ffffff')
            sel_dir_btn.pack(side=tk.LEFT, padx=(0, 10))
            tk.Label(btn_frame, text='选择此文件夹', bg='#ffffff', fg='#999999',
                     font=_sao_font(8)).pack(side=tk.LEFT, padx=(0, 18))

        ok_btn = _make_aa_icon_button(btn_frame, 'ok', self._confirm,
                                      SAOColors.OK_BLUE, SAOColors.OK_BLUE, bg='#ffffff')
        ok_btn.pack(side=tk.LEFT, padx=20)

        tk.Label(btn_frame, text='确认', bg='#ffffff', fg='#999999',
                 font=_sao_font(8)).pack(side=tk.LEFT, padx=(0, 20))

        cancel_btn = _make_aa_icon_button(btn_frame, 'close', self._cancel,
                          SAOColors.CLOSE_RED, SAOColors.CLOSE_RED, bg='#ffffff')
        cancel_btn.pack(side=tk.LEFT, padx=(0, 4))

        tk.Label(btn_frame, text='取消', bg='#ffffff', fg='#999999',
                 font=_sao_font(8)).pack(side=tk.LEFT)

    def _start_drag(self, e):
        self._drag['x'] = e.x_root
        self._drag['y'] = e.y_root

    def _do_drag(self, e):
        dx = e.x_root - self._drag['x']
        dy = e.y_root - self._drag['y']
        self.geometry(f'+{self.winfo_x() + dx}+{self.winfo_y() + dy}')
        self._drag['x'] = e.x_root
        self._drag['y'] = e.y_root

    def _load_dir(self, path):
        self._current_dir = os.path.abspath(path)
        self._path_lbl.configure(text=self._current_dir)
        self._listbox.delete(0, tk.END)
        self._entries = [('..', True)]
        self._listbox.insert(tk.END, '▴ ..')
        self._listbox.itemconfig(0, fg=self._ACCENT2)

        try:
            entries = sorted(os.listdir(self._current_dir))
        except PermissionError:
            entries = []

        dirs = [e for e in entries if os.path.isdir(os.path.join(self._current_dir, e))]
        files = [e for e in entries if os.path.isfile(os.path.join(self._current_dir, e))]

        exts = set()
        for _, pattern in self._filetypes:
            for p in pattern.split(';'):
                p = p.strip()
                if p.startswith('*.'):
                    exts.add(p[1:].lower())
                elif p == '*.*':
                    exts = None
                    break
            if exts is None:
                break

        for d in dirs:
            if not d.startswith('.'):
                idx = self._listbox.size()
                self._listbox.insert(tk.END, f'▸ {d}')
                self._listbox.itemconfig(idx, fg=self._DIR_FG)
                self._entries.append((d, True))

        for f in files:
            if exts is None or any(f.lower().endswith(ext) for ext in exts):
                idx = self._listbox.size()
                self._listbox.insert(tk.END, f'♪ {f}')
                self._listbox.itemconfig(idx, fg=self._FILE_FG)
                self._entries.append((f, False))

    def _on_select(self, e):
        sel = self._listbox.curselection()
        if not sel:
            return
        name, is_dir = self._entries[sel[0]]
        if not is_dir and hasattr(self, '_fname_lbl'):
            self._fname_lbl.configure(text=name)

    def _on_double_click(self, e):
        sel = self._listbox.curselection()
        if not sel:
            return
        name, is_dir = self._entries[sel[0]]
        full = os.path.join(self._current_dir, name)
        if is_dir:
            self._load_dir(full)
        else:
            self.result = full
            self._finish()

    def _confirm(self):
        sel = self._listbox.curselection()
        if sel:
            name, is_dir = self._entries[sel[0]]
            if is_dir:
                if self._mode == 'dir' and name != '..':
                    # 目录模式: 确认选中的子文件夹
                    self.result = os.path.join(self._current_dir, name)
                    self._finish()
                else:
                    self._load_dir(os.path.join(self._current_dir, name))
            else:
                self.result = os.path.join(self._current_dir, name)
                self._finish()

    def _confirm_dir(self):
        """目录模式: 选择当前浏览的文件夹"""
        self.result = self._current_dir
        self._finish()

    def _cancel(self):
        self.result = None
        self._finish()

    def _finish(self):
        result = self.result
        callback = self.callback
        parent = self.master
        try:
            self.grab_release()
        except Exception:
            pass
        # 收起动画 (反向 520→135px, 300ms)
        self._animate_collapse(result, callback, parent)

    def _animate_collapse(self, result, callback, parent):
        """SAO 风格收起动画 (宽度 → 135px, 300ms ease-in)."""
        import time as _time
        t0 = _time.time()
        dur = 0.3
        fw = self._final_w
        iw = self._initial_w
        try:
            cx = self.winfo_x() + self.winfo_width() // 2
            cy = self.winfo_y()
        except Exception:
            cx = self._px + fw // 2
            cy = self._py

        def _step():
            if not self.winfo_exists():
                if callback and result:
                    try:
                        parent.after(50, lambda: callback(result))
                    except Exception:
                        callback(result)
                return
            elapsed = _time.time() - t0
            t = min(1.0, elapsed / dur)
            et = t ** 2  # ease-in quad
            w = int(fw - (fw - iw) * et)
            x = cx - w // 2
            self.geometry(f'{w}x{self._final_h}+{x}+{cy}')
            try:
                self.attributes('-alpha', max(0.0, 1.0 - t))
            except Exception:
                pass
            if t < 1.0:
                self.after(16, _step)
            else:
                self.destroy()
                if callback and result:
                    try:
                        parent.after(50, lambda: callback(result))
                    except Exception:
                        callback(result)
        _step()


