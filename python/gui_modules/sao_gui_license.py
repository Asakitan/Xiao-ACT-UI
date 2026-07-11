# -*- coding: utf-8 -*-
# sao_gui_license — SAO 风格授权验证弹窗 (Tk 端)
#
# 启动时由 main.py / sao_gui.py 调用, 显示当前授权状态。
# 未授权用户可输入激活码; 免费用户可跳过 (仅引擎 A)。
from __future__ import annotations

import ctypes
import threading
import time
import tkinter as tk
from typing import Callable, Optional

from utils.sao_sound import get_sao_font as _sao_font, get_cjk_font as _cjk_font
from sao_theme.colors import SAOColors
from sao_theme.animator import Animator
from sao_theme.utils import ease_out, lerp, _make_aa_icon_button
from gui_modules.sao_panel_ui import _remember_sao_panel_root, _theme_color

# ── 尺寸 ──
_FINAL_W = 460
_FINAL_H = 380
_INITIAL_W = 135
_HEADER_H = 62
_FOOTER_H = 78

# ── 配色 ──
_BG_OUTER = '#e0e0e0'
_BG_WHITE = '#ffffff'
_BG_CONTENT = '#eae9e9'
_FG_TITLE = SAOColors.ALERT_TITLE_FG     # #646364
_FG_CONTENT = '#888888'
_FG_LABEL = '#a0a0a0'
_FG_VALUE = '#3b3a3c'
_FG_FREE = '#dea620'
_FG_PAID = '#3fae5a'
_FG_DANGER = '#ef684e'
_INPUT_BORDER = '#d1d1d6'
_INPUT_CURSOR = '#f3af12'
_ACCENT_GOLD = '#f3af12'
_ACCENT_CYAN = '#68e4ff'


def show_license_dialog(parent: Optional[tk.Tk] = None,
                        on_done: Optional[Callable] = None):
    # 弹出 SAO 风格授权验证面板。
    #
    # Args:
    # parent: Tk root (可为 None, 会临时创建)
    # on_done: 面板关闭后回调 (无论激活与否)
    own_root = False
    if parent is None:
        parent = tk.Tk()
        parent.withdraw()
        own_root = True

    from license import get_license_manager, TIER_FREE, TIER_PAID
    mgr = get_license_manager()
    outer_bg = _theme_color('bg', _BG_OUTER)
    surface_bg = _theme_color('card_bg', _BG_WHITE)
    content_bg = _theme_color('body_bg', _BG_CONTENT)
    title_fg = _theme_color('value_fg', _FG_TITLE)
    label_fg = _theme_color('label_fg', _FG_LABEL)
    value_fg = _theme_color('value_fg', _FG_VALUE)
    input_border = _theme_color('border', _INPUT_BORDER)
    input_cursor = _theme_color('gold', _INPUT_CURSOR)
    accent_gold = _theme_color('gold', _ACCENT_GOLD)
    accent_cyan = _theme_color('accent', _ACCENT_CYAN)
    separator = _theme_color('sep', '#d8dde2')

    dlg = tk.Toplevel(parent)
    _remember_sao_panel_root(dlg)
    dlg.overrideredirect(True)
    dlg.attributes('-topmost', True)
    dlg.configure(bg=outer_bg)

    # DWM 圆角
    try:
        dlg.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(dlg.winfo_id())
        val = ctypes.c_int(2)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(val), 4)
    except Exception:
        pass

    # 居中
    px = (dlg.winfo_screenwidth() - _FINAL_W) // 2
    py = (dlg.winfo_screenheight() - _FINAL_H) // 2
    dlg.geometry(f'{_INITIAL_W}x{_FINAL_H}+{px + (_FINAL_W - _INITIAL_W) // 2}+{py}')

    # ── 主容器 ──
    main = tk.Frame(dlg, bg=surface_bg)
    main.pack(fill=tk.BOTH, expand=True)
    main.pack_forget()

    # ── 标题栏 ──
    header = tk.Frame(main, bg=surface_bg, height=_HEADER_H)
    header.pack(fill=tk.X)
    header.pack_propagate(False)

    # 左侧 cyan 装饰条
    tk.Frame(header, bg=accent_cyan, width=3, height=20).pack(
        side=tk.LEFT, padx=(14, 0), pady=(_HEADER_H // 2 - 10, 0))
    title_lbl = tk.Label(header, text='', bg=surface_bg,
                         fg=title_fg, font=_sao_font(13, True))
    title_lbl.pack(side=tk.LEFT, padx=(8, 0), expand=False)

    # 右侧 gold 装饰
    tk.Frame(header, bg=accent_gold, width=18, height=2).pack(
        side=tk.RIGHT, padx=(0, 14), pady=(_HEADER_H // 2, 0))

    tk.Frame(main, bg=separator, height=1).pack(fill=tk.X)

    # ── 内容区 ──
    content = tk.Frame(main, bg=content_bg)
    content.pack(fill=tk.BOTH, expand=True, padx=0, pady=0)

    inner = tk.Frame(content, bg=content_bg)
    inner.pack(fill=tk.BOTH, expand=True, padx=24, pady=16)

    # 状态信息行
    status_frame = tk.Frame(inner, bg=content_bg)
    status_frame.pack(fill=tk.X, pady=(0, 12))

    # 授权等级
    tier_row = tk.Frame(status_frame, bg=content_bg)
    tier_row.pack(fill=tk.X, pady=2)
    tk.Label(tier_row, text='授权等级', bg=content_bg,
             fg=label_fg, font=_cjk_font(9), anchor='w').pack(side=tk.LEFT)
    tier_val = tk.Label(tier_row, text='', bg=content_bg,
                        fg=value_fg, font=_sao_font(11, True), anchor='e')
    tier_val.pack(side=tk.RIGHT)

    # 引擎权限
    engine_row = tk.Frame(status_frame, bg=content_bg)
    engine_row.pack(fill=tk.X, pady=2)
    tk.Label(engine_row, text='可用引擎', bg=content_bg,
             fg=label_fg, font=_cjk_font(9), anchor='w').pack(side=tk.LEFT)
    engine_val = tk.Label(engine_row, text='', bg=content_bg,
                          fg=value_fg, font=_sao_font(10, True), anchor='e')
    engine_val.pack(side=tk.RIGHT)

    # HWID
    hwid_row = tk.Frame(status_frame, bg=content_bg)
    hwid_row.pack(fill=tk.X, pady=2)
    tk.Label(hwid_row, text='设备指纹', bg=content_bg,
             fg=label_fg, font=_cjk_font(9), anchor='w').pack(side=tk.LEFT)
    hwid_val = tk.Label(hwid_row, text='', bg=content_bg,
                        fg=label_fg, font=_sao_font(8), anchor='e')
    hwid_val.pack(side=tk.RIGHT)

    # 分割线
    tk.Frame(inner, bg=separator, height=1).pack(fill=tk.X, pady=(4, 10))

    # 激活码输入
    input_label = tk.Label(inner, text='输入激活码', bg=content_bg,
                           fg=label_fg, font=_cjk_font(9), anchor='w')
    input_label.pack(fill=tk.X)

    entry_border = tk.Frame(inner, bg=input_border, bd=0)
    entry_border.pack(fill=tk.X, ipady=1, pady=(4, 0))
    key_entry = tk.Entry(entry_border, font=('Segoe UI', 12),
                         bg=surface_bg, fg=value_fg,
                         relief='flat', bd=0,
                         insertbackground=input_cursor)
    key_entry.pack(fill=tk.X, padx=2, pady=2, ipady=6)

    # 消息提示
    msg_lbl = tk.Label(inner, text='', bg=content_bg,
                       fg=_FG_DANGER, font=_cjk_font(8),
                       anchor='w', wraplength=_FINAL_W - 72)
    msg_lbl.pack(fill=tk.X, pady=(6, 0))

    tk.Frame(main, bg=separator, height=1).pack(fill=tk.X)

    # ── 底部按钮区 ──
    footer = tk.Frame(main, bg=surface_bg, height=_FOOTER_H)
    footer.pack(fill=tk.X)
    footer.pack_propagate(False)

    btn_box = tk.Frame(footer, bg=surface_bg)
    btn_box.place(relx=0.5, rely=0.5, anchor='center')

    _result = {'closed': False}

    def _refresh_status():
        status = mgr.get_status()
        tier = status['tier']
        if tier == TIER_PAID:
            tier_val.configure(text='付费版', fg=_FG_PAID)
            engine_val.configure(text='A · B · C · D', fg=_FG_PAID)
        else:
            tier_val.configure(text='免费版', fg=_FG_FREE)
            engine_val.configure(text='A', fg=_FG_FREE)
        hwid_val.configure(text=status['hwid'])

    def _close():
        if _result['closed']:
            return
        _result['closed'] = True
        _do_close_anim(dlg, on_done, own_root and parent)

    def _do_activate():
        key = key_entry.get().strip()
        if not key:
            msg_lbl.configure(text='请输入激活码', fg=_FG_DANGER)
            return

        msg_lbl.configure(text='验证中...', fg=_FG_LABEL)
        dlg.update_idletasks()

        def _worker():
            result = mgr.activate(key)
            dlg.after(0, lambda: _on_result(result))

        threading.Thread(target=_worker, daemon=True).start()

    def _on_result(result):
        if result.get('ok'):
            msg_lbl.configure(text='激活成功！', fg=_FG_PAID)
            _refresh_status()
            dlg.after(800, _close)
        else:
            msg_lbl.configure(text=result.get('error', '激活失败'), fg=_FG_DANGER)
            entry_border.configure(bg=_FG_DANGER)
            dlg.after(1200, lambda: entry_border.configure(bg=input_border))

    def _skip():
        _dismiss_license_dialog()
        msg_lbl.configure(text='')
        _close()

    # 激活按钮 (蓝)
    ok_btn = _make_aa_icon_button(btn_box, 'ok', _do_activate,
                                  SAOColors.OK_BLUE, SAOColors.OK_BLUE,
                                     bg=surface_bg)
    ok_btn.pack(side=tk.LEFT, padx=16)

    # 跳过按钮 (红)
    skip_btn = _make_aa_icon_button(btn_box, 'close', _skip,
                                    SAOColors.CLOSE_RED, SAOColors.CLOSE_RED,
                                       bg=surface_bg)
    skip_btn.pack(side=tk.LEFT, padx=16)

    # Enter 激活
    key_entry.bind('<Return>', lambda e: _do_activate())

    # ── 展开动画 ──
    anim = Animator(dlg)

    def expand(t):
        if not dlg.winfo_exists():
            return
        w = int(lerp(_INITIAL_W, _FINAL_W, t))
        x = px + (_FINAL_W - w) // 2
        dlg.geometry(f'{w}x{_FINAL_H}+{x}+{py}')

    def reveal():
        if not main.winfo_manager():
            main.pack(fill=tk.BOTH, expand=True)
            dlg.update_idletasks()
        _clip_reveal_text(title_lbl, '授 权 验 证', dlg, 400, delay=80)
        _refresh_status()

    anim.animate('expand', 500, expand, on_done=reveal)

    # ── 拖拽 ──
    _drag = {'x': 0, 'y': 0}
    def start_drag(e):
        _drag['x'], _drag['y'] = e.x_root, e.y_root
    def do_drag(e):
        dx, dy = e.x_root - _drag['x'], e.y_root - _drag['y']
        dlg.geometry(f'+{dlg.winfo_x() + dx}+{dlg.winfo_y() + dy}')
        _drag['x'], _drag['y'] = e.x_root, e.y_root
    for w in [header, title_lbl]:
        w.bind('<Button-1>', start_drag)
        w.bind('<B1-Motion>', do_drag)

    dlg.focus_force()
    key_entry.focus_set()

    if own_root:
        parent.mainloop()

    return dlg


def _clip_reveal_text(label, text, dlg, duration_ms, delay=0):
    def start():
        if not dlg.winfo_exists():
            return
        steps = max(1, duration_ms // 30)
        step = [0]
        def tick():
            if not dlg.winfo_exists():
                return
            t = min(step[0] / steps, 1.0)
            n = len(text)
            visible = int(n * t)
            s = (n - visible) // 2
            e = s + visible
            label.configure(text=' ' * s + text[s:e] + ' ' * (n - e))
            step[0] += 1
            if t < 1.0:
                dlg.after(30, tick)
            else:
                label.configure(text=text)
        tick()
    if delay > 0:
        dlg.after(delay, start)
    else:
        start()


def _do_close_anim(dlg, on_done=None, destroy_root=None):
    if not dlg.winfo_exists():
        if on_done:
            on_done()
        return

    try:
        dlg.grab_release()
    except Exception:
        pass

    cur_w = dlg.winfo_width()
    cur_h = dlg.winfo_height()
    cur_x = dlg.winfo_x()
    cur_y = dlg.winfo_y()
    anim = Animator(dlg)

    def shrink(t):
        if not dlg.winfo_exists():
            return
        w = max(1, int(lerp(cur_w, 0, t)))
        x = cur_x + (cur_w - w) // 2
        try:
            dlg.geometry(f'{w}x{cur_h}+{x}+{cur_y}')
            dlg.attributes('-alpha', 1.0 - t)
        except Exception:
            pass

    def finish():
        try:
            if dlg.winfo_exists():
                dlg.destroy()
        except Exception:
            pass
        if destroy_root:
            try:
                destroy_root.destroy()
            except Exception:
                pass
        if on_done:
            on_done()

    anim.animate('close', 350, shrink, on_done=finish)


# ── 持久化: 用户跳过后不再弹出 ──

_DISMISS_KEY = 'license_dialog_dismissed'


def _dismiss_license_dialog():
    try:
        from config import SettingsManager
        s = SettingsManager()
        s.set(_DISMISS_KEY, True)
        s.save()
    except Exception:
        pass


def is_license_dialog_dismissed() -> bool:
    try:
        from config import SettingsManager
        return bool(SettingsManager().get(_DISMISS_KEY, False))
    except Exception:
        return False


def reset_license_dialog_dismissed():
    try:
        from config import SettingsManager
        s = SettingsManager()
        s.set(_DISMISS_KEY, False)
        s.save()
    except Exception:
        pass
