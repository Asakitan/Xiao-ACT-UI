# -*- coding: utf-8 -*-
"""
SAO Utils 风格完整 GUI — 独立 UI 壳
包含 SAO PopUpMenu 菜单系统, SAO Alert 对话框, HP 血条进度条,
LINK START 入场动画, SAO 风格文件选择器
"""

import tkinter as tk
from tkinter import ttk
import os
import sys
import json
import ctypes
import math
import time
import threading
from typing import Optional

from PIL import Image, ImageDraw, ImageTk, ImageFilter, ImageFont
import numpy as np

from config import (
    APP_VERSION_LABEL, WINDOW_TITLE, WINDOW_SIZE,
    DEFAULT_HOTKEYS,
    get_skill_slot_rects,
)
from sao_theme import (
    SAOColors, SAOButton, SAOProgressBar, SAOTitleBar, SAODialog,
    SAOLeaderboardDialog,
    SAOStatusPill, SAOResizeGrip, SAOFilePicker, SAOSeparator,
    SAOPopUpMenu, SAOHPBar, SAOLinkStart, SAOCircleButton,
    Animator, lerp, lerp_color, ease_out, ease_in_out
)
from character_profile import (
    load_profile, save_profile, get_or_ask_profile,
    show_welcome_dialog, PROFESSION_LIST,
    calc_level, add_song_xp
)
from sao_sound import play_sound, LevelUpEffect, load_sao_fonts, get_sao_font, get_cjk_font
from auto_key_engine import (
    AutoKeyEngine,
    build_auto_key_state,
    build_identity_state,
    default_upload_auth_state,
    load_auto_key_config,
    save_auto_key_config,
    snapshot_author_from_state,
)
from boss_raid_engine import (
    BossRaidEngine,
    build_boss_raid_state,
    load_boss_raid_config,
    save_boss_raid_config,
)
from boss_autokey_linkage import (
    BossAutoKeyLinkage,
    load_linkage_config,
    save_linkage_config,
)
from dps_tracker import DpsTracker
from sao_gui_dps import DpsOverlay
from sao_gui_bosshp import BossHpOverlay
from sao_gui_alert import AlertOverlay
from sao_gui_autokey import AutoKeyPanel
from sao_gui_bossraid import BossRaidPanel


class ModernColors:
    BG_DARK = '#1C1C1E'; BG_CARD = '#2C2C2E'; BG_HOVER = '#3A3A3C'
    BG_INPUT = '#1C1C1E'; BG_PANEL = '#2C2C2E'
    ACCENT_BLUE = '#0A84FF'; ACCENT_GREEN = '#30D158'; ACCENT_RED = '#FF453A'
    ACCENT_ORANGE = '#FF9F0A'; ACCENT_PURPLE = '#BF5AF2'; ACCENT_CYAN = '#64D2FF'
    ACCENT_PINK = '#FF375F'
    TEXT_PRIMARY = '#F5F5F7'; TEXT_SECONDARY = '#98989D'; TEXT_BRIGHT = '#FFFFFF'
    TEXT_DIM = '#636366'
    ROW_HIGH = '#323236'; ROW_MID_HIGH = '#2E3230'; ROW_MID = '#302E34'; ROW_CHORD = '#342E2E'
    KEY_NORMAL = '#3A3A3C'; KEY_PRESSED = '#0A84FF'; KEY_BORDER = '#48484A'
    VIZ_LOW = '#30D158'; VIZ_MID = '#64D2FF'; VIZ_HIGH = '#0A84FF'; VIZ_TOP = '#BF5AF2'
    BTN_PRIMARY = '#0A84FF'; BTN_SECONDARY = '#48484A'; BTN_DANGER = '#FF453A'
    BORDER = '#38383A'; BORDER_BRIGHT = '#48484A'
    GLOW_BLUE = '#1A5AFF'; GLOW_CYAN = '#28C8FF'
    TITLEBAR = '#161618'; VIZ_BG = '#131315'
    PIANO_WHITE = '#DCDCE0'; PIANO_BLACK = '#2A2A2E'; PIANO_BG = '#1A1A1C'
    @classmethod
    def apply_theme(cls, name): pass
    @classmethod
    def current_theme(cls): return 'dark'
    @classmethod
    def toggle_theme(cls): pass
    @classmethod
    def is_sao(cls): return True

class SmoothButton(tk.Canvas):
    def __init__(self, parent, text="", command=None, width=100, height=34,
                 bg=None, fg="#FFFFFF", radius=8, font_size=11, **kwargs):
        super().__init__(parent, width=width, height=height,
                         highlightthickness=0, bd=0, **kwargs)

class MidiVisualizer(tk.Frame):
    NUM_BARS = 36; BAR_DECAY = 0.90; UPDATE_INTERVAL = 33
    def __init__(self, parent, settings=None, **kwargs):
        super().__init__(parent, **kwargs)
        self._canvas = tk.Canvas(self, bg='#131315', highlightthickness=0)
        self._canvas.pack(fill='both', expand=True)
    def feed_note(self, *a, **kw): pass
    def set_mode(self, *a, **kw): pass
    def start(self, *a, **kw): pass
    def stop(self, *a, **kw): pass
    def trigger_note(self, *a, **kw): pass

KEYBOARD_LAYOUT = {'row1': [], 'row2': [], 'row3': []}
NOTE_NAMES = {'row1': [], 'row2': [], 'row3': []}
BLACK_KEY_LAYOUT = {}
BLACK_KEY_NAMES = {}
NOTE_NAMES_EXTENDED = {'row1': [], 'row2': [], 'row3': []}
BLACK_KEY_NAMES_EXTENDED = {}
KEY_TO_MIDI = {}
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'settings.json')

# pyglet Link Start 渲染器 (已弃用, 保留文件但不再使用)
# OpenGL 上下文要求主线程, 与 tkinter 冲突, 改用 Canvas SAO-UI 隧道模型
HAS_PYGLET = False

# ── 全局快捷键检测 (复用 gui.py 逻辑) ──
def _is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

KEYBOARD_HOTKEY_AVAILABLE = False
KEYBOARD_ERROR_MSG = None
try:
    import keyboard as kb
    try:
        _tc = lambda: None
        kb.add_hotkey('ctrl+alt+shift+f12', _tc, suppress=False)
        kb.remove_hotkey('ctrl+alt+shift+f12')
        KEYBOARD_HOTKEY_AVAILABLE = True
    except Exception as e:
        KEYBOARD_ERROR_MSG = str(e)
except ImportError:
    KEYBOARD_ERROR_MSG = "未安装keyboard库"

PYNPUT_HOTKEY_AVAILABLE = False
try:
    from pynput import keyboard as pynput_kb
    from pynput.keyboard import Key, KeyCode
    PYNPUT_HOTKEY_AVAILABLE = True
except ImportError:
    pass

GLOBAL_HOTKEY_AVAILABLE = PYNPUT_HOTKEY_AVAILABLE or KEYBOARD_HOTKEY_AVAILABLE


# ══════════════════════════════════════════════════════════
#  Settings Manager (与 gui.py 共享 settings.json)
# ══════════════════════════════════════════════════════════
class SettingsManager:
    def __init__(self):
        self.settings = {
            'hotkeys': DEFAULT_HOTKEYS.copy(),
            'ui_mode': 'sao',
        }
        self.load()

    def load(self):
        try:
            if os.path.exists(CONFIG_FILE):
                with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
                    self.settings.update(json.load(f))
        except:
            pass

    def save(self):
        try:
            for legacy_key in ('last_file', 'speed', 'transpose', 'chord_mode'):
                self.settings.pop(legacy_key, None)
            with open(CONFIG_FILE, 'w', encoding='utf-8') as f:
                json.dump(self.settings, f, indent=2, ensure_ascii=False)
        except:
            pass

    def get(self, key, default=None):
        return self.settings.get(key, default)

    def set(self, key, value):
        self.settings[key] = value
        self.save()


def _get_icon_path():
    if getattr(sys, 'frozen', False):
        base = sys._MEIPASS if hasattr(sys, '_MEIPASS') else os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(__file__))
    p = os.path.join(base, 'icon.ico')
    return p if os.path.exists(p) else None


def _set_process_app_id(app_id: str):
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
    except Exception:
        pass


def _apply_window_icon(win):
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


def _apply_panel_style(panel):
    """为浮动 Toplevel 面板添加 DWM 圆角 + 系统阴影 — 增强浮动质感"""
    try:
        panel.update_idletasks()
        hwnd = int(_user32.GetParent(ctypes.c_void_p(panel.winfo_id())))
        # DWM 圆角 (Win11+)
        val = ctypes.c_int(2)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(val), 4)
        # 系统阴影 (CS_DROPSHADOW)
        GCL_STYLE = -26
        CS_DROPSHADOW = 0x00020000
        cls = ctypes.windll.user32.GetClassLongW(hwnd, GCL_STYLE)
        ctypes.windll.user32.SetClassLongW(hwnd, GCL_STYLE, cls | CS_DROPSHADOW)
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


def _apply_viz_light_theme(viz, sao_colors=None):
    """No-op stub — visualizer theme removed."""
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


def _make_sao_panel_hud(parent, width: int, height: int, alpha: float = 0.18):
    """生成一个轻量 SAO HUD 画布，提供左右错层飘移装饰。"""
    cv = tk.Canvas(parent, width=width, height=height, bg=parent.cget('bg'),
                   highlightthickness=0, bd=0)
    cv.place(x=0, y=0, relwidth=1, relheight=1)
    cv.tk.call('lower', cv._w)
    return cv


def _hex_rgba(hex_color: str, alpha: int = 255):
    hex_color = hex_color.lstrip('#')
    if len(hex_color) == 3:
        hex_color = ''.join(ch * 2 for ch in hex_color)
    return tuple(int(hex_color[i:i+2], 16) for i in (0, 2, 4)) + (alpha,)


def _make_panel_close_button(parent, command, bg=_SAO_PANEL_HEADER_BG):
    size = 18
    scale = 4
    sw = size * scale
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

    lbl = tk.Label(parent, bg=bg, image=normal, cursor='hand2', bd=0, highlightthickness=0)
    lbl._img_normal = normal
    lbl._img_hover = hover
    lbl.configure(image=normal)
    lbl.bind('<Enter>', lambda e: lbl.configure(image=lbl._img_hover))
    lbl.bind('<Leave>', lambda e: lbl.configure(image=lbl._img_normal))
    lbl.bind('<Button-1>', lambda e: command())
    return lbl


# ══════════════════════════════════════════════════════════
#  Win32 per-pixel alpha 分层窗口 (UpdateLayeredWindow)
# ══════════════════════════════════════════════════════════

class _BLENDFUNCTION(ctypes.Structure):
    _fields_ = [('BlendOp', ctypes.c_byte), ('BlendFlags', ctypes.c_byte),
                ('SourceConstantAlpha', ctypes.c_byte), ('AlphaFormat', ctypes.c_byte)]

class _ULW_SIZE(ctypes.Structure):
    _fields_ = [('cx', ctypes.c_long), ('cy', ctypes.c_long)]

class _ULW_POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', ctypes.c_ulong), ('biWidth', ctypes.c_long),
        ('biHeight', ctypes.c_long), ('biPlanes', ctypes.c_ushort),
        ('biBitCount', ctypes.c_ushort), ('biCompression', ctypes.c_ulong),
        ('biSizeImage', ctypes.c_ulong), ('biXPelsPerMeter', ctypes.c_long),
        ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', ctypes.c_ulong),
        ('biClrImportant', ctypes.c_ulong),
    ]


# Win32 函数签名 (64-bit 安全, 防止 HWND/HDC 截断)
_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32
for _fn, _res, _args in [
    (_user32.GetDC, ctypes.c_void_p, [ctypes.c_void_p]),
    (_user32.ReleaseDC, ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p]),
    (_user32.GetParent, ctypes.c_void_p, [ctypes.c_void_p]),
    (_user32.GetWindowLongW, ctypes.c_long, [ctypes.c_void_p, ctypes.c_int]),
    (_user32.SetWindowLongW, ctypes.c_long, [ctypes.c_void_p, ctypes.c_int, ctypes.c_long]),
    (_user32.UpdateLayeredWindow, ctypes.c_int,
        [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
         ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong]),
    (_gdi32.CreateCompatibleDC, ctypes.c_void_p, [ctypes.c_void_p]),
    (_gdi32.CreateDIBSection, ctypes.c_void_p,
        [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
         ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_uint]),
    (_gdi32.SelectObject, ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_void_p]),
    (_gdi32.DeleteObject, ctypes.c_int, [ctypes.c_void_p]),
    (_gdi32.DeleteDC, ctypes.c_int, [ctypes.c_void_p]),
]:
    _fn.restype = _res
    _fn.argtypes = _args
del _fn, _res, _args


def _update_layered_win(hwnd, rgba_image, overall_alpha=255, dst_pos=None):
    """使用 Win32 UpdateLayeredWindow 实现真正的逐像素 alpha 透明窗口。
    rgba_image: PIL RGBA Image;  overall_alpha: 0-255 整体 alpha。"""
    if not hwnd:
        return False
    w, h = rgba_image.size
    # RGBA -> 预乘 BGRA (ULW 要求预乘 alpha)
    arr = np.array(rgba_image, dtype=np.float32)
    a = arr[:, :, 3:4] / 255.0
    bgra = np.empty((h, w, 4), dtype=np.uint8)
    bgra[:, :, 0] = np.clip(arr[:, :, 2] * a[:, :, 0], 0, 255)  # B
    bgra[:, :, 1] = np.clip(arr[:, :, 1] * a[:, :, 0], 0, 255)  # G
    bgra[:, :, 2] = np.clip(arr[:, :, 0] * a[:, :, 0], 0, 255)  # R
    bgra[:, :, 3] = arr[:, :, 3].astype(np.uint8)

    bmi = _BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(bmi)
    bmi.biWidth = w
    bmi.biHeight = -h   # top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32

    hdcS = _user32.GetDC(None)
    hdcM = _gdi32.CreateCompatibleDC(hdcS)
    bits = ctypes.c_void_p()
    hbmp = _gdi32.CreateDIBSection(hdcM, ctypes.byref(bmi), 0,
                                    ctypes.byref(bits), None, 0)
    if not hbmp:
        _gdi32.DeleteDC(hdcM)
        _user32.ReleaseDC(None, hdcS)
        return False
    old = _gdi32.SelectObject(hdcM, hbmp)
    raw = bgra.tobytes()
    ctypes.memmove(bits, raw, len(raw))

    pt = _ULW_POINT(0, 0)
    sz = _ULW_SIZE(w, h)
    bf = _BLENDFUNCTION(0, 0, min(255, max(0, int(overall_alpha))), 1)
    dst = None
    if dst_pos is not None:
        try:
            dx, dy = dst_pos
            dst = _ULW_POINT(int(dx), int(dy))
        except Exception:
            dst = None
    ok = _user32.UpdateLayeredWindow(
        ctypes.c_void_p(hwnd), hdcS, ctypes.byref(dst) if dst is not None else None, ctypes.byref(sz),
        hdcM, ctypes.byref(pt), 0, ctypes.byref(bf), 2)

    _gdi32.SelectObject(hdcM, old)
    _gdi32.DeleteObject(hbmp)
    _gdi32.DeleteDC(hdcM)
    _user32.ReleaseDC(None, hdcS)
    return bool(ok)


def _get_hp_pil_font(size, family='sao', _cache={}):
    """加载 PIL 字体用于 HP 条渲染 (带缓存)。"""
    key = (family, size)
    if key in _cache:
        return _cache[key]
    base = os.path.dirname(os.path.abspath(__file__))
    fname = 'SAOUI.ttf' if family == 'sao' else 'ZhuZiAYuanJWD.ttf'
    fp = os.path.join(base, 'assets', 'fonts', fname)
    try:
        font = ImageFont.truetype(fp, size=size)
    except Exception:
        font = ImageFont.load_default()
    _cache[key] = font
    return font


# ══════════════════════════════════════════════════════════
#  迷你钢琴 (60键可视化) — 精简版
# ══════════════════════════════════════════════════════════
class SAOMiniPiano(tk.Canvas):
    """No-op stub — MIDI piano removed."""
    def __init__(self, *a, **kw): super().__init__(*a, **kw)
    def note_on(self, *a, **kw): pass
    def reset(self, *a, **kw): pass

class SAOHotkeyManager:
    """全局快捷键管理 (与 gui.py HotkeyPanel 逻辑一致)"""

    # F键虚拟键码表 (Windows VK codes)
    _FKEY_VK = {
        'F1': 112, 'F2': 113, 'F3': 114, 'F4': 115,
        'F5': 116, 'F6': 117, 'F7': 118, 'F8': 119,
        'F9': 120, 'F10': 121, 'F11': 122, 'F12': 123,
    }

    def __init__(self, settings: SettingsManager, actions: dict):
        self.settings = settings
        self.actions = actions
        self._listener = None
        self._pressed_keys = set()
        self._start()

    def _start(self):
        if not PYNPUT_HOTKEY_AVAILABLE:
            return
        try:
            self._listener = pynput_kb.Listener(
                on_press=self._on_press, on_release=self._on_release)
            self._listener.daemon = True
            self._listener.start()
        except Exception:
            pass

    def _on_press(self, key):
        try:
            if isinstance(key, KeyCode) and key.vk:
                self._pressed_keys.add(key.vk)
            elif isinstance(key, Key):
                self._pressed_keys.add(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except:
            pass
        self._check_combos()

    def _on_release(self, key):
        try:
            if isinstance(key, KeyCode) and key.vk:
                self._pressed_keys.discard(key.vk)
            elif isinstance(key, Key):
                self._pressed_keys.discard(key.value.vk if hasattr(key.value, 'vk') else str(key))
        except:
            pass

    def _check_combos(self):
        saved = self.settings.get('hotkeys', DEFAULT_HOTKEYS)
        for action, info in saved.items():
            vk = None
            if isinstance(info, dict):
                vk = info.get('vk')
            elif isinstance(info, str) and info:
                vk = self._FKEY_VK.get(info.upper())
            if vk and vk in self._pressed_keys:
                cb = self.actions.get(action)
                if cb:
                    cb()
                    self._pressed_keys.clear()
                    return

    def cleanup(self):
        if self._listener:
            try:
                self._listener.stop()
            except:
                pass


# ══════════════════════════════════════════════════════════
#  SAO Player GUI — 完整独立 UI
# ══════════════════════════════════════════════════════════
#  SAO 左侧玩家信息面板 (替代 SAOLeftInfo)
#  对标 SAO-UI HP 组件 + LeftInfo 组件
# ══════════════════════════════════════════════════════════
class SAOPlayerPanel(tk.Frame):
    """
    SAO 风格左侧信息面板 — 对标 SAO-UI LeftInfo + HP 组件
    
    结构:
    - Top 区 (白色, 240×280): 用户名/分隔线/HP条/等级/文件信息
    - Bottom 区 (灰色, 240×120): 描述 + 状态信息
    - 右三角指示器 (连接 MenuBar)
    - 下三角装饰 (连接 top/bottom)
    """

    def __init__(self, parent, username='Player', profession='', **kw):
        super().__init__(parent, bg=parent.cget('bg'), highlightthickness=0, **kw)
        self._active = False
        self._anim = Animator(self)
        self._target_w = 240
        self._top_h = 240
        self._bottom_h = 80

        # 用户资料
        self._username = username
        self._profession = profession

        # 等级数据
        self._level = 1
        self._xp_percent = 0.0  # 当前经验百分比 (0~1)
        self._xp_total = 0  # 累计 XP (用于 calc_level)

        # 播放数据
        self._file_name = "未选择文件"
        self._status = "就绪"
        self._time_current = 0
        self._time_total = 0
        self._speed = 1.0
        self._transpose = 0
        self._hp_percent = 1.0
        self._hp_current = 1000
        self._hp_total = 1000
        self._mode = "经典60键"
        self._shift_mode = "普通模式"
        self._bpm = 0
        self._is_playing = False
        self._sustain = False

        self._build()

    def _build(self):
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

    def update_file(self, name):
        self._file_name = name
        if self._active:
            self._redraw_top(self._target_w, self._top_h)

    def update_progress(self, current, total):
        self._time_current = current
        self._time_total = total
        if total > 0:
            self._hp_percent = current / total
            self._hp_current = int(current)
            self._hp_total = int(total)
        if self._active:
            self._redraw_top(self._target_w, self._top_h)

    def update_status(self, status, is_playing=None):
        self._status = status
        if is_playing is not None:
            self._is_playing = is_playing
        if self._active:
            self._redraw_bottom(self._target_w, self._bottom_h)

    def update_speed(self, speed):
        self._speed = speed
        if self._active:
            self._redraw_top(self._target_w, self._top_h)

    def update_transpose(self, t):
        self._transpose = t
        if self._active:
            self._redraw_top(self._target_w, self._top_h)

    def update_mode(self, mode_text):
        self._mode = mode_text
        if self._active:
            self._redraw_bottom(self._target_w, self._bottom_h)

    def update_bpm(self, bpm):
        self._bpm = bpm
        if self._active:
            self._redraw_top(self._target_w, self._top_h)

    def update_sustain(self, on: bool):
        self._sustain = on
        if self._active:
            self._redraw_bottom(self._target_w, self._bottom_h)

    def update_shift_mode(self, mode_text: str):
        self._shift_mode = mode_text
        if self._active:
            self._redraw_bottom(self._target_w, self._bottom_h)

    def update_level(self, level: int, xp_pct: float, xp_total: int = 0):
        """更新等级信息"""
        self._level = level
        self._xp_percent = xp_pct
        self._xp_total = xp_total
        if self._active:
            self._redraw_top(self._target_w, self._top_h)

    def _animate_open(self):
        def phase1(t):
            w = max(1, int(self._target_w * t))
            h = max(1, int(self._top_h * t))
            self._top.configure(width=w, height=h)
            self._redraw_top(w, h)

        def phase2(t):
            h = max(1, int(self._bottom_h * t))
            self._bottom.configure(width=self._target_w, height=h)
            self._redraw_bottom(self._target_w, h)

        self._anim.animate('top_open', 500, phase1,
                           on_done=lambda: self._anim.animate('bottom_open', 400, phase2))

    def _animate_close(self):
        def fade(t):
            inv = 1 - t
            w = max(1, int(self._target_w * inv))
            self._top.configure(width=w, height=max(1, int(self._top_h * inv)))
            self._bottom.configure(width=w, height=max(1, int(self._bottom_h * inv)))

        self._anim.animate('close', 200, fade)

    def _redraw_top(self, w, h):
        """SAO 系统信息面板 .top — HUD 风格"""
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
        self._top.create_text(w // 2, 84,
                              text=f'Lv. {self._level}',
                              font=get_sao_font(20, True), fill=GOLD)

        if h < 120:
            return

        # ── 经验值条 ──
        from character_profile import calc_level as _cl
        _lv, _cur_xp, _need_xp = _cl(
            getattr(self, '_xp_total', 0) if hasattr(self, '_xp_total') else 0)

        # EXP 标签
        self._top.create_text(20, 108, text='EXP', anchor='w',
                              font=get_sao_font(7), fill=LABEL)
        self._top.create_text(w - 20, 108, text=f'{_cur_xp} / {_need_xp}',
                              anchor='e', font=get_sao_font(8), fill='#999999')

        # 经验条 (带边框)
        if h > 124:
            xp_y = 122
            xp_x = 20
            xp_w = w - 40
            xp_h = 6
            # 底色
            self._top.create_rectangle(xp_x, xp_y, xp_x + xp_w, xp_y + xp_h,
                                       fill='#e8e8e8', outline='#d8d8d8', width=1)
            xp_fill = int(xp_w * self._xp_percent)
            if xp_fill > 0:
                self._top.create_rectangle(xp_x + 1, xp_y + 1,
                                           xp_x + xp_fill, xp_y + xp_h - 1,
                                           fill=GOLD, outline='')
                # 光泽高亮
                self._top.create_rectangle(xp_x + 1, xp_y + 1,
                                           xp_x + xp_fill, xp_y + 3,
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
            t = time.time()
            scan_x = 10 + int((w - 20) * ((math.sin(t * 1.5) + 1) / 2))
            self._top.create_rectangle(scan_x - 12, scan_y - 1, scan_x + 12, scan_y + 1,
                                       fill=CYAN, outline='')

    def _redraw_bottom(self, w, h):
        """SAO 系统信息面板 .bottom — 状态描述区"""
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

        # ── 键位模式 + 延音状态 ──
        sm = self._shift_mode if self._shift_mode else '普通模式'
        sm_color = '#2196f3' if sm == '普通模式' else ('#e65100' if 'CTRL' in sm else '#1565c0')
        self._bottom.create_text(15, h // 2 - 2, text=sm,
                                 font=get_cjk_font(9, True), fill=sm_color,
                                 anchor='w')
        # 延音指示
        sus_text = '延音 ON' if self._sustain else ''
        if sus_text:
            self._bottom.create_text(w - 12, h // 2 - 2, text=sus_text,
                                     font=get_sao_font(7, True), fill='#3ad86c',
                                     anchor='e')

        # 底部系统标签
        if h > 50:
            self._bottom.create_text(w - 8, h - 10, text='SAO://SYSTEM',
                                     anchor='e', font=get_sao_font(5), fill='#c8c8c8')



# ══════════════════════════════════════════════════════════
#  SAO Player GUI — 纯悬浮 SAO Menu 架构
# ══════════════════════════════════════════════════════════
class SAOPlayerGUI:
    """
    纯悬浮 SAO Utils 风格 GUI — 没有传统窗口！
    - 常驻: 小型悬浮触发按钮 (Toplevel)
    - 展开: SAO PopUpMenu 全屏菜单 = 主界面
    - 左面板: SAOPlayerPanel (文件/进度/状态)
    - 菜单按钮: 5 类 (文件/播放/设置/控制/关于)
    - 子菜单: 所有播放控制
    - 可选: 浮动钢琴/可视化面板
    """

    def __init__(self):
        _set_process_app_id('sao.auto.game.ui')
        self.root = tk.Tk()
        self.root.withdraw()  # root 永远隐藏, 只作为 Tk 事件循环
        self.root.title("SAO Auto — 游戏辅助 UI")

        self.settings = SettingsManager()
        # 记录当前 UI 模式 — 下次启动时使用
        self.settings.set('ui_mode', 'sao')
        self.settings.save()

        # ── 角色配置 ──
        profile = load_profile()
        self._username = profile.get('username', '')
        self._profession = profile.get('profession', '')
        self._level = profile.get('level', 1)
        self._xp = profile.get('xp', 0)
        self._songs_played = profile.get('songs_played', 0)
        self._play_time = profile.get('play_time', 0)

        # 加载 SAO 字体
        load_sao_fonts()

        self._current_file = None
        self._panels_hidden = False  # 一键隐藏所有面板
        self._hidden_panels_snapshot = []  # 隐藏前记录哪些面板是开的
        self._player_panel = None  # 当 SAO 菜单打开时设置
        self._picker = None        # SAOFilePicker 引用 (防止 GC)
        self._piano_panel = None   # 浮动钢琴面板
        self._viz_panel = None     # 浮动可视化面板
        self._status_panel = None  # 浮动状态面板
        self._control_panel = None # 浮动控制面板
        self._fisheye_ov = None    # 菜单开启时的持久鱼眼叠加层
        self._ctx_menu_open = False  # 右键菜单弹出中, 暂停 z-order 置顶
        self._mini_piano = None
        self._visualizer = None
        self._lift_loop_active = False
        self._skip_canvas_click = False
        self._float_progress_pct = 0.0
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []
        self._float_hud_ids = []
        self._float_hud_text = []
        self._destroyed = False  # hot-switch 守卫: 阻止 after() 回调在 root 销毁后执行
        self._exit_animating = False
        self._close_finalized = False
        self._entry_overlay = None
        self._exit_overlay = None
        # 浮动呼吸动画
        self._breath_active = False
        self._breath_base_x = 0
        self._breath_base_y = 0
        self._breath_t0 = 0.0

        # ── 体力覆盖板 (stamina overlay) ──
        self._stamina_win = None
        self._stamina_hwnd = 0
        self._sta_w = 0
        self._sta_h = 0
        self._sta_hp = (0, 1)
        self._sta_sta = (0, 1)

        # ── 识别引擎 ──
        self._recognition_active = False
        self._recognition_engine = None
        self._recognition_engines = []
        self._packet_engine = None
        self._vision_engine = None
        self._vision_paused_for_death = False
        self._last_dead_state = False
        self._state_mgr = None
        self._game_state = None
        self._cfg_settings_ref = None
        self._recog_lock = threading.Lock()

        # ── AutoKey / BossRaid / DPS 引擎 ──
        self._auto_key_engine = None
        self._boss_raid_engine = None
        self._boss_autokey_linkage = None
        self._dps_tracker = None
        self._dps_visible = True
        self._dps_faded = False
        self._last_burst_ready = False
        self._last_burst_slot = 0
        self._last_boss_timer_text = ''
        self._last_boss_timer_urgency = ''
        self._last_boss_bar_sig = None
        self._last_skillfx_sig = None
        self._profile_auto_saved = False
        self._last_gs_name = ''
        self._last_gs_prof = ''
        self._last_gs_uid = ''

        # ── ULW 覆盖层引用 ──
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._alert_overlay = None

        # ── 配置面板实例 ──
        self._autokey_panel = None    # AutoKeyPanel
        self._bossraid_panel = None   # BossRaidPanel

        self._set_icon()
        self._create_floating_widget()
        self._setup_sao_menu()
        self._setup_hotkeys()

        # LINK START 入场
        self.root.after(100, self._play_link_start)

    def _set_icon(self):
        _set_process_app_id('sao.auto.game.ui')
        _apply_window_icon(self.root)
        # icon.ico 应用到 root (所有子窗口自动继承)

    def _create_hp_alpha_strip_windows(self):
        """(ULW 模式下 HP 填充已由 PIL alpha 梯度渲染, 不再需要条带窗口)"""
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []

    def _destroy_hp_alpha_strip_windows(self):
        for item in getattr(self, '_hp_alpha_windows', []):
            try:
                item['win'].destroy()
            except Exception:
                pass
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []

    def _render_hp_strip_image(self, *a, **kw):
        return None

    def _sync_hp_alpha_strip_windows(self):
        """(ULW 模式下不需要同步条带窗口)"""
        pass

    def _build_float_hud_items(self):
        """(ULW 模式下 HUD 已统一由 PIL 渲染, 此方法保留接口兼容)"""
        pass

    def _render_hp_shell(self, hover=False, scale=4):
        """渲染静态 HP 外壳 + 身份面板底板为 RGBA PIL Image (4× 超采样 + LANCZOS)。"""
        FW, FH = self._fw, self._fh
        ox, oy = self._hp_ox, self._hp_oy
        BW, BH = 505, 48
        xt_w = 26
        xr_x = ox + xt_w + 3
        xr_w = BW - xt_w - 3
        bar_x = ox + 110
        bar_y = oy + 10
        PW, PT, PH, PS = 350, 19, 27, 145
        num_x = ox + int(BW * 0.60)
        num_y = oy + int(BH * 0.90)
        xp_w = int(170 * 0.69)
        lv_x = num_x + xp_w + 3
        lv_w = int(170 * 0.30)

        bg = '#cfd0c5' if hover else '#cfd0c5'
        border = '#b4b6aa' if hover else '#b4b6aa'
        glow_cyan = '#9cecff' if hover else '#86dfff'
        glow_gold = '#ffd06a' if hover else '#f3af12'

        sw, sh = FW * scale, FH * scale
        img = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        def S(v):
            return int(round(v * scale))

        def P(pts):
            return [(S(x), S(y)) for x, y in pts]

        # ── 不透明底板: 覆盖 HP+STA 通过 SAO 形状元素自身实现 ──
        # (无额外矩形, 避免覆盖技能栏)

        # ── xt_left 方块 ──
        draw.rectangle((S(ox), S(oy), S(ox + xt_w), S(oy + BH)), fill=_hex_rgba(bg, 255))
        draw.rectangle((S(ox), S(oy + BH / 4), S(ox + xt_w / 2), S(oy + BH * 3 / 4)),
                        fill=(0, 0, 0, 0))

        # ── xt_right 异形多边形 ──
        xr_pts = [
            (xr_x + 75, oy + int(BH * 0.22)),
            (xr_x + xr_w, oy + int(BH * 0.22)),
            (xr_x + xr_w, oy),
            (xr_x, oy), (xr_x, oy + BH),
            (xr_x + 210, oy + BH),
            (xr_x + 210, oy + int(BH * 0.80)),
            (xr_x + xr_w, oy + int(BH * 0.80)),
            (xr_x + xr_w, oy + int(BH * 0.60)),
            (xr_x + 200, oy + int(BH * 0.60)),
            (xr_x + 195, oy + int(BH * 0.77)),
            (xr_x + 75, oy + int(BH * 0.77)),
        ]
        draw.polygon(P(xr_pts), fill=_hex_rgba(bg, 252))

        # ── 右侧渐隐条纹 ──
        fade_start = xr_x + int(xr_w * 0.40)
        fade_end = xr_x + xr_w
        n_strips = 72
        bg_rgb = _hex_rgba(bg, 255)
        for i in range(n_strips):
            t = i / max(1, n_strips - 1)
            alpha = int(248 * (1.0 - t * t))
            sx = fade_start + (fade_end - fade_start) * i / n_strips
            ex = fade_start + (fade_end - fade_start) * (i + 1) / n_strips + 1
            fill = (bg_rgb[0], bg_rgb[1], bg_rgb[2], alpha)
            draw.rectangle((S(sx), S(oy), S(ex), S(oy + BH * 0.22)), fill=fill)
            draw.rectangle((S(sx), S(oy + BH * 0.60), S(ex), S(oy + BH * 0.80)), fill=fill)

        # ── HP 条边框 ──
        bar_pts = [
            (bar_x, bar_y), (bar_x + PW, bar_y),
            (bar_x + PW - 5, bar_y + PT), (bar_x + PS, bar_y + PT),
            (bar_x + PS - 4, bar_y + PH), (bar_x, bar_y + PH),
        ]
        draw.polygon(P(bar_pts), fill=(180, 182, 170, 255))
        draw.line(P(bar_pts + [bar_pts[0]]),
                  fill=_hex_rgba(border, 255), width=max(1, scale))
        draw.line(P([(bar_x + 2, bar_y + 1), (bar_x + PW - 2, bar_y + 1)]),
                  fill=_hex_rgba(border, 220), width=max(1, scale))
        draw.line(P([(bar_x + 2, bar_y + PH - 1), (bar_x + PS - 5, bar_y + PH - 1)]),
                  fill=_hex_rgba(border, 220), width=max(1, scale))
        draw.polygon(P([
            (bar_x + 2, bar_y + 2), (bar_x + PW - 3, bar_y + 2),
            (bar_x + PW - 8, bar_y + PT - 1), (bar_x + PS + 1, bar_y + PT - 1),
            (bar_x + PS - 6, bar_y + PH - 3), (bar_x + 2, bar_y + PH - 3),
        ]), fill=(207, 208, 197, 120))

        # ── 数值底框 ──
        draw.rectangle((S(num_x), S(num_y), S(num_x + xp_w), S(num_y + 20)),
                        fill=_hex_rgba(bg, 250))
        draw.rectangle((S(lv_x), S(num_y), S(lv_x + lv_w), S(num_y + 20)),
                        fill=_hex_rgba(bg, 250))

        # ── 身份面板底板 (从 ~4%FW 开始, 无间隙衔接 HP 条) ──
        id_x_start = max(4, int(FW * 0.04))   # 与 HUD x-offset 对齐
        id_y = 0               # 从画布顶部开始, 填满整个面板
        id_h = FH - 2          # 延伸到画布底部
        id_right = ox          # 无间隙, 直接衔接 HP 条左端
        # 带角度的 SAO 风格多边形
        id_pts = [
            (id_x_start, id_y),
            (int(id_right * 0.96), id_y),
            (id_right, id_y + int(id_h * 0.28)),
            (id_right, FH - 2),
            (id_x_start, FH - 2),
        ]
        draw.polygon(P(id_pts), fill=(207, 208, 197, 255))
        draw.polygon(P(id_pts), outline=(180, 182, 170, 200))
        # SAO 角标 (左上青色, 右下金色)
        clen = S(6)
        draw.line([(S(id_x_start + 1), S(id_y + 1)), (S(id_x_start + 1) + clen, S(id_y + 1))], fill=(104, 228, 255, 140), width=max(1, scale))
        draw.line([(S(id_x_start + 1), S(id_y + 1)), (S(id_x_start + 1), S(id_y + 1) + clen)], fill=(104, 228, 255, 140), width=max(1, scale))
        draw.line([(S(id_right - 4), S(FH - 4)), (S(id_right - 4) - clen, S(FH - 4))], fill=(212, 156, 23, 120), width=max(1, scale))
        draw.line([(S(id_right - 4), S(FH - 4)), (S(id_right - 4), S(FH - 4) - clen)], fill=(212, 156, 23, 120), width=max(1, scale))

        # ── STA 条底板 (HP 下方, +12px 更低) ──
        sta_x = ox + 28
        sta_y = oy + BH + 16
        sta_track_w = 460
        sta_h = 8
        # 不透明磨砂轨道底 (完全覆盖原生STA)
        draw.rectangle((S(sta_x), S(sta_y), S(sta_x + sta_track_w), S(sta_y + sta_h)),
                        fill=(207, 208, 197, 255), outline=(180, 182, 170, 200))

        return img.resize((FW, FH), Image.LANCZOS)

    def _render_hp_dynamic(self):
        """合成完整 HP 帧: 静态外壳 + HP 填充 + 文字 + 身份面板 + STA 条。"""
        FW, FH = self._fw, self._fh
        shell = self._hp_shell_hover.copy() if self._hp_hover else self._hp_shell_normal.copy()
        draw = ImageDraw.Draw(shell)

        ox, oy = self._hp_ox, self._hp_oy

        # ── HP 填充 (带从左到右的 alpha 梯度) ──
        pct = self._float_progress_pct
        if pct > 0:
            if pct >= 0.60:
                c = (154, 211, 52)
            elif pct >= 0.25:
                c = (244, 250, 73)
            else:
                c = (239, 104, 78)
            top_max_w = self._hp_bar_right - self._hp_bar_x
            top_fill_w = max(1, int(top_max_w * pct))
            bot_max_w = self._hp_bar_step_x - self._hp_bar_x
            bot_fill_w = min(top_fill_w, bot_max_w)
            alpha_base = 1.0

            # 上层 — 完全不透明 HP 填充
            h_top = max(1, self._hp_bar_bot_top - self._hp_bar_y)
            grad_top = np.zeros((h_top, top_fill_w, 4), dtype=np.uint8)
            grad_top[:, :, :3] = c
            grad_top[:, :, 3] = 255
            fill_top_img = Image.fromarray(grad_top)
            shell.paste(fill_top_img, (self._hp_bar_x, self._hp_bar_y), fill_top_img)

            # 下层 — 完全不透明 HP 填充
            if bot_fill_w > 0:
                h_bot = max(1, self._hp_bar_bot_full - self._hp_bar_bot_top)
                grad_bot = np.zeros((h_bot, bot_fill_w, 4), dtype=np.uint8)
                grad_bot[:, :, :3] = c
                grad_bot[:, :, 3] = 255
                fill_bot_img = Image.fromarray(grad_bot)
                shell.paste(fill_bot_img, (self._hp_bar_x, self._hp_bar_bot_top), fill_bot_img)

        # ── 文字: 用户名 (XTBox 内) ──
        name = getattr(self, '_hp_display_name', 'Player')
        BW, BH = 505, 48
        xr_x = ox + 26 + 3
        name_cx = (xr_x + xr_x + 85) // 2
        name_cy = oy + BH // 2
        try:
            fn = _get_hp_pil_font(16, 'cjk')
            draw.text((name_cx, name_cy), name, fill=(60, 62, 50, 255),
                      font=fn, anchor='mm')
        except Exception:
            draw.text((name_cx - 10, name_cy - 6), name, fill=(60, 62, 50, 255))

        # ── 文字: XP / 等级 ──
        num_x = ox + int(BW * 0.60)
        num_y = oy + int(BH * 0.90)
        xp_w = int(170 * 0.69)
        lv_x = num_x + xp_w + 3
        lv_w = int(170 * 0.30)
        try:
            fn_num = _get_hp_pil_font(14, 'sao')
            if self._playing or self._paused:
                # 播放中/暂停中: 显示时间 mm:ss / mm:ss
                tc = getattr(self, '_time_current', 0)
                tt = getattr(self, '_time_total', 0)
                cur_m, cur_s = int(tc) // 60, int(tc) % 60
                tot_m, tot_s = int(tt) // 60, int(tt) % 60
                time_str = f'{cur_m:02d}:{cur_s:02d}/{tot_m:02d}:{tot_s:02d}'
                draw.text((num_x + xp_w - 5, num_y + 9),
                          time_str, fill=(88, 152, 190, 255),
                          font=fn_num, anchor='rm')
                draw.text((lv_x + lv_w - 5, num_y + 9),
                          f'lv.{self._level}', fill=(60, 62, 50, 255),
                          font=fn_num, anchor='rm')
            else:
                gs = getattr(self, '_game_state', None)
                if self._recognition_active and gs is not None:
                    # 识别模式: 显示真实 HP 和等级
                    if gs.hp_max > 0:
                        hp_c, hp_m = self._sta_hp
                        draw.text((num_x + xp_w - 5, num_y + 9),
                                  f'{hp_c}/{hp_m}', fill=(88, 152, 190, 255),
                                  font=fn_num, anchor='rm')
                    else:
                        _lv, _cur_xp, _need_xp = calc_level(self._xp)
                        draw.text((num_x + xp_w - 5, num_y + 9),
                                  f'{_cur_xp}/{_need_xp}', fill=(60, 62, 50, 255),
                                  font=fn_num, anchor='rm')
                    # 等级: 优先显示 (+XX) 部分，否则显示 level_base
                    if gs.level_extra > 0:
                        lv_disp = gs.level_extra
                    elif gs.level_base > 0:
                        lv_disp = gs.level_base
                    else:
                        lv_disp = self._level
                    draw.text((lv_x + lv_w - 5, num_y + 9),
                              f'lv.{lv_disp}', fill=(60, 62, 50, 255),
                              font=fn_num, anchor='rm')
                else:
                    _lv, _cur_xp, _need_xp = calc_level(self._xp)
                    draw.text((num_x + xp_w - 5, num_y + 9),
                              f'{_cur_xp}/{_need_xp}', fill=(60, 62, 50, 255),
                              font=fn_num, anchor='rm')
                    draw.text((lv_x + lv_w - 5, num_y + 9),
                              f'lv.{self._level}', fill=(60, 62, 50, 255),
                              font=fn_num, anchor='rm')
        except Exception:
            pass

        # ── 身份面板文字 (左侧 — frosted-glass 暗橄榄色文字) ──
        try:
            id_x_start = max(4, int(FW * 0.04))
            id_y = oy - 4
            id_h = FH - id_y - 2
            id_right = ox - 2
            fn_sys = _get_hp_pil_font(8, 'sao')
            fn_name = _get_hp_pil_font(15, 'cjk')
            fn_lv = _get_hp_pil_font(12, 'sao')
            fn_link = _get_hp_pil_font(8, 'sao')
            fn_uid = _get_hp_pil_font(7, 'sao')
            # ── 使用完整面板高度 (匹配 shell 底板) ──
            id_y = 0
            id_h = FH - 2
            id_right = ox
            # ── 读取 GameState 数据 ──
            gs = getattr(self, '_game_state', None)
            gs_ok = self._recognition_active and gs is not None
            # 等级文本
            if gs_ok:
                if gs.level_extra > 0 and gs.level_base > 0:
                    lv_txt = f'Lv.{gs.level_base}(+{gs.level_extra})'
                elif gs.level_extra > 0:
                    lv_txt = f'Lv.{gs.level_extra}'
                elif gs.level_base > 0:
                    lv_txt = f'Lv.{gs.level_base}'
                else:
                    lv_txt = f'Lv.{self._level}'
            else:
                lv_txt = f'Lv.{self._level}'
            # 职业
            prof_txt = ''
            if gs_ok and gs.profession_name:
                prof_txt = gs.profession_name
            # UID
            uid_txt = ''
            if gs_ok and gs.player_id:
                uid_txt = f'UID: {gs.player_id}'

            # SYSTEM 标签
            draw.text((id_x_start + 12, id_y + int(id_h * 0.06)), 'SYSTEM',
                      fill=(97, 98, 86, 140), font=fn_sys, anchor='lm')
            # 玩家名
            draw.text((id_x_start + 12, id_y + int(id_h * 0.20)), name,
                      fill=(60, 62, 50, 255), font=fn_name, anchor='lm')
            # 职业
            if prof_txt:
                draw.text((id_x_start + 12, id_y + int(id_h * 0.34)), prof_txt,
                          fill=(88, 152, 190, 230), font=fn_lv, anchor='lm')
            # 等级
            lv_y_pos = 0.44 if prof_txt else 0.34
            draw.text((id_x_start + 12, id_y + int(id_h * lv_y_pos)), lv_txt,
                      fill=(212, 156, 23, 230), font=fn_lv, anchor='lm')
            # UID
            if uid_txt:
                draw.text((id_x_start + 12, id_y + int(id_h * 0.56)), uid_txt,
                          fill=(97, 98, 86, 180), font=fn_uid, anchor='lm')
            # NErVGear — LINK OK
            draw.text((id_x_start + 12, id_y + int(id_h * 0.70)),
                      'NErVGear \u2500 LINK OK',
                      fill=(88, 152, 190, 200), font=fn_link, anchor='lm')
            # LINKRATE (模拟连接状态)
            draw.text((id_x_start + 12, id_y + int(id_h * 0.84)),
                      'LINKRATE: 100.0%',
                      fill=(154, 211, 52, 160), font=fn_uid, anchor='lm')
        except Exception:
            pass

        # ── STA 条填充 (金色, HP 下方) ──
        try:
            sta_cur, sta_max = self._sta_sta
            sta_x = ox + 28
            sta_y_bar = oy + 48 + 16   # BH=48, +12px lower
            sta_track_w = 460
            sta_h = 8
            sta_pct = sta_cur / max(1, sta_max)
            sta_fill_w = max(0, int(sta_track_w * sta_pct))
            if sta_fill_w > 0:
                draw.rectangle((sta_x + 1, sta_y_bar + 1, sta_x + sta_fill_w, sta_y_bar + sta_h - 1),
                                fill=(243, 175, 18, 255))
                # 高光
                draw.line([(sta_x + 1, sta_y_bar + 1), (sta_x + sta_fill_w, sta_y_bar + 1)],
                          fill=(255, 220, 100, 100))
            # STA 标签 + 数值
            fn_sta = _get_hp_pil_font(9, 'sao')
            draw.text((sta_x - 24, sta_y_bar + sta_h // 2), 'STA',
                      fill=(212, 156, 23, 220), font=fn_sta, anchor='lm')
            draw.text((sta_x + sta_track_w + 6, sta_y_bar + sta_h // 2),
                      f'{sta_cur}/{sta_max}',
                      fill=(97, 98, 86, 180), font=fn_sta, anchor='lm')
        except Exception:
            pass

        # ── 技能栏已移至 WebView — 此处禁用渲染 ──
        # (Burst Mode Ready animation replaces the old skill bar grid)

        return shell

    def _refresh_hp_layered(self):
        """重新渲染并更新分层 HP 窗口。"""
        if self._destroyed and not self._exit_animating:
            return
        try:
            if not self._float or not self._float.winfo_exists():
                return
        except Exception:
            return
        if not self._float_hwnd:
            return
        alpha = getattr(self, '_float_alpha', 0.92)
        try:
            if alpha <= 0.01:
                # alpha ≈ 0: 仍然调用 ULW (全透明), 防止 Tk 黑底暴露
                _blank = Image.new('RGBA', (self._fw, self._fh), (0, 0, 0, 0))
                _update_layered_win(self._float_hwnd, _blank, 0)
                return
            img = self._render_hp_dynamic()
            dst_pos = None
            try:
                dst_pos = (self._float.winfo_rootx(), self._float.winfo_rooty())
            except Exception:
                pass
            ok = _update_layered_win(self._float_hwnd, img, int(255 * alpha), dst_pos=dst_pos)
            if not ok and not getattr(self, '_ulw_warned', False):
                self._ulw_warned = True
                print(f'[SAO-HP] UpdateLayeredWindow FAILED, hwnd=0x{self._float_hwnd:X}')
        except Exception as e:
            if not getattr(self, '_ulw_warned', False):
                self._ulw_warned = True
                print(f'[SAO-HP] _refresh_hp_layered error: {e}')
                import traceback; traceback.print_exc()

    def _set_float_alpha(self, alpha):
        """设置 HP 窗口整体透明度并刷新。"""
        self._float_alpha = alpha
        self._refresh_hp_layered()

    def _animate_float_hud(self):
        """30fps HUD 呼吸动画循环 — 每帧重新渲染分层窗口。"""
        if self._destroyed:
            return
        try:
            if not self._float.winfo_exists():
                return
        except Exception:
            return
        self._refresh_hp_layered()
        try:
            self.root.after(33, self._animate_float_hud)
        except Exception:
            pass

    def _attach_sao_panel_fx(self, panel, header, inner, accent='#86dfff'):
        """给 Tk 浮动面板附加 SAO 风格 HUD 背景和左右错层漂移。"""
        try:
            panel.update_idletasks()
            pw = max(80, panel.winfo_width())
            ph = max(60, panel.winfo_height())
        except Exception:
            return

        if getattr(panel, '_sao_fx_inited', False):
            return
        panel._sao_fx_inited = True
        header_cv = _make_sao_panel_hud(header, pw, 24)
        body_cv = _make_sao_panel_hud(inner, pw, ph)
        panel._sao_header_hud = header_cv
        panel._sao_body_hud = body_cv

        def _tick():
            try:
                if self._destroyed or not panel.winfo_exists():
                    return
            except Exception:
                return
            tt = time.time() + (hash(str(panel)) % 17) * 0.13
            header_cv.delete('all')
            body_cv.delete('all')
            cyan = '#86dfff'
            gold = '#f3af12'

            left_far = int(10 + 5 * math.sin(tt * 0.66))
            left_near = int(20 + 12 * math.sin(tt * 1.35 + 0.8))
            right_far = int(pw - 18 + 7 * math.sin(tt * 0.72 + 1.1))
            right_near = int(pw - 34 + 12 * math.sin(tt * 1.45 + 2.1))
            body_cv.create_line(left_far, 30, left_far + 78, 30, fill=cyan, width=1)
            body_cv.create_line(left_near, ph - 44, left_near + 102, ph - 44, fill=gold, width=1)
            body_cv.create_line(right_far - 88, 42, right_far, 42, fill=cyan, width=1)
            body_cv.create_line(right_near - 110, ph - 58, right_near, ph - 58, fill=gold, width=1)
            for i in range(5):
                lx = left_far + i * 12
                rx2 = right_far - i * 13
                body_cv.create_line(lx, 48, lx, 54 + (i % 2) * 3, fill=cyan, width=1)
                body_cv.create_line(rx2, ph - 74, rx2, ph - 68 - (i % 2) * 3, fill=gold, width=1)
            body_cv.create_rectangle(left_near + 8, ph - 36, left_near + 66, ph - 24, outline=cyan, width=1)
            body_cv.create_rectangle(right_near - 74, 22, right_near - 12, 34, outline=gold, width=1)
            try:
                self.root.after(33, _tick)
            except Exception:
                pass

        _tick()

    # ══════════════════════════════════════════════
    #  悬浮触发按钮 — 纯 SAO-UI HP 组件 (对标 HP/src/index.vue)
    # ══════════════════════════════════════════════
    def _create_floating_widget(self):
        """SAO-UI 统一 HUD — 覆盖身份面板 + HP + STA 区域。
        使用 UpdateLayeredWindow 实现真正的逐像素 alpha 透明，
        所有内容(外壳/HP填充/身份/STA/文字)统一由 PIL 渲染后一次性刷新。
        """
        try:
            _sw = self.root.winfo_screenwidth()
            _sh = self.root.winfo_screenheight()
        except Exception:
            _sw, _sh = 1920, 1080

        # ── 统一 HUD 尺寸: 75% 屏宽, 高 140px (覆盖原生 HP+STA) ──
        FW = int(_sw * 0.75)
        FH = 140
        self._fw, self._fh = FW, FH
        self._float_alpha = 0.0
        self._hp_hover = False

        # HP XTBox 偏移: 44% 对齐游戏 HP 条 (下移 + 右移)
        self._hp_ox = int(FW * 0.44)
        self._hp_oy = 38
        # 身份面板宽度: ~40% 窗口 (覆盖游戏左下角信息)
        self._id_plate_w = int(FW * 0.40)

        self._float = tk.Toplevel(self.root)
        self._float.overrideredirect(True)
        self._float.attributes('-topmost', True)
        self._float.geometry(f'{FW}x{FH}')
        self._float.configure(bg='#000000')
        _apply_window_icon(self._float)

        # Win32: 设为分层窗口 + 任务栏可见
        self._float_hwnd = 0
        try:
            self._float.update_idletasks()
            GWL_EXSTYLE = -20
            WS_EX_APPWINDOW = 0x00040000
            WS_EX_TOOLWINDOW = 0x00000080
            WS_EX_LAYERED = 0x00080000
            hwnd = int(_user32.GetParent(ctypes.c_void_p(self._float.winfo_id())))
            self._float_hwnd = hwnd
            style = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
            style = (style | WS_EX_APPWINDOW | WS_EX_LAYERED) & ~WS_EX_TOOLWINDOW
            _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE, style)
            _disable_native_window_shadow(self._float)
            # 立即绘制一帧全透明 ULW, 让 Win32 接管渲染 (Tk 黑底永不显示)
            try:
                _blank = Image.new('RGBA', (FW, FH), (0, 0, 0, 0))
                _update_layered_win(hwnd, _blank, 0)
            except Exception:
                pass
            print(f'[SAO-HP] ULW hwnd=0x{hwnd:X}, layered OK')
        except Exception as e:
            print(f'[SAO-HP] ULW init FAILED: {e}')
            import traceback; traceback.print_exc()
            self._float_hwnd = 0

        # ── 预渲染静态外壳 (normal / hover) ──
        self._hp_shell_normal = self._render_hp_shell(hover=False)
        self._hp_shell_hover = self._render_hp_shell(hover=True)

        # ── HP 布局常量 (相对于 hp_ox/hp_oy) ──
        ox, oy = self._hp_ox, self._hp_oy
        bar_x = ox + 110; bar_y = oy + 10
        PW, PT, PH, PS = 350, 19, 27, 145
        self._hp_bar_x        = bar_x + 2
        self._hp_bar_y        = bar_y + 2
        self._hp_bar_right    = bar_x + PW - 3
        self._hp_bar_bot_top  = bar_y + PT - 1
        self._hp_bar_bot_full = bar_y + PH - 2
        self._hp_bar_step_x   = bar_x + PS - 2

        # ── 显示名 ──
        display_name = self._username if self._username else 'Player'
        if len(display_name) > 10:
            display_name = display_name[:9] + '…'
        self._hp_display_name = display_name

        # ── 点击交互 (拖拽已禁用 — 固定位置) ──
        self._float.bind('<Button-1>', lambda e: self._toggle_sao_menu())
        self._float.bind('<Enter>', self._float_enter)
        self._float.bind('<Leave>', self._float_leave)

        # 右键菜单 (SAO Auto — 精简, 深色, 向上弹出)
        self._float_ctx = tk.Menu(self._float, tearoff=0,
                                  bg='#0f121a', fg='#d0e8f0',
                                  activebackground='#f3af12',
                                  activeforeground='#ffffff',
                                  relief='flat', bd=1,
                                  font=get_cjk_font(9))
        self._float_ctx.add_command(label='◆ 打开 SAO 菜单', command=self._toggle_sao_menu)
        self._float_ctx.add_separator()
        self._float_ctx.add_command(label='◉ 状态面板', command=self._toggle_status_panel)
        self._float_ctx.add_command(label='⚡ AutoKey 配置', command=self._toggle_autokey_panel)
        self._float_ctx.add_command(label='⚔ BossRaid 配置', command=self._toggle_bossraid_panel)
        self._float_ctx.add_separator()
        self._float_ctx.add_command(label='◈ 隐藏/显示面板', command=self._toggle_hide_all_panels)
        self._float_ctx.add_command(label='◇ WebView UI', command=self._switch_to_webview_ui)
        self._float_ctx.add_command(label='✕ 退出', command=self._on_close)
        def _show_ctx_menu(e):
            self._ctx_menu_open = True
            try:
                # 在点击位置上方弹出菜单
                menu_h = 165
                popup_x = e.x_root
                popup_y = max(0, e.y_root - menu_h)
                self._float_ctx.tk_popup(popup_x, popup_y)
            except Exception:
                self._float_ctx.tk_popup(e.x_root, e.y_root)
            finally:
                self._ctx_menu_open = False
        self._float.bind('<Button-3>', _show_ctx_menu)

        # 初始渲染一次 (alpha=0，不可见)
        try:
            self._refresh_hp_layered()
        except Exception:
            pass

        # 初始隐藏 — LinkStart 完成后才显示
        self._float.withdraw()

    # ══════════════════════════════════════════════
    #  体力覆盖板 (Stamina Overlay — SAO style ULW)
    # ══════════════════════════════════════════════
    def _create_stamina_overlay(self):
        """创建体力/HP 覆盖板窗口 — SAO 风格, 覆盖游戏原生 HP/STA 区域."""
        try:
            _sw = self.root.winfo_screenwidth()
            _sh = self.root.winfo_screenheight()
        except Exception:
            _sw, _sh = 1920, 1080

        # 位置: 屏幕下方中央 (覆盖游戏原生 生命值/体力值 条)
        sta_w = self.settings.get('sta_width', int(_sw * 0.36))
        sta_h = self.settings.get('sta_height', 76)
        sta_x = self.settings.get('sta_fixed_x', int((_sw - sta_w) * 0.5))
        sta_y = self.settings.get('sta_fixed_y', int(_sh - sta_h - 1))
        self._sta_w, self._sta_h = sta_w, sta_h

        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.attributes('-topmost', True)
        win.geometry(f'{sta_w}x{sta_h}+{sta_x}+{sta_y}')
        win.configure(bg='#000000')
        _apply_window_icon(win)
        win.withdraw()

        hwnd = 0
        try:
            win.update_idletasks()
            GWL_EXSTYLE = -20
            WS_EX_LAYERED = 0x00080000
            WS_EX_TOOLWINDOW = 0x00000080
            h = int(_user32.GetParent(ctypes.c_void_p(win.winfo_id())))
            hwnd = h
            style = _user32.GetWindowLongW(ctypes.c_void_p(h), GWL_EXSTYLE)
            style = (style | WS_EX_LAYERED | WS_EX_TOOLWINDOW)
            _user32.SetWindowLongW(ctypes.c_void_p(h), GWL_EXSTYLE, style)
            _blank = Image.new('RGBA', (sta_w, sta_h), (0, 0, 0, 0))
            _update_layered_win(h, _blank, 0)
            print(f'[SAO-STA] ULW hwnd=0x{h:X}, layered OK  ({sta_w}x{sta_h})')
        except Exception as e:
            print(f'[SAO-STA] ULW init FAILED: {e}')
            hwnd = 0

        self._stamina_win = win
        self._stamina_hwnd = hwnd

    def _render_stamina_frame(self):
        """PIL 渲染覆盖板 — 不透明 SAO 系统 HUD。
        上: SYSTEM / LINK STATUS / NErVGEAR / 时钟
        下: STA 金条 + 数值
        """
        W, H = self._sta_w, self._sta_h
        if W < 10 or H < 10:
            return Image.new('RGBA', (max(10, W), max(10, H)), (0, 0, 0, 0))

        scale = 2
        sw, sh = W * scale, H * scale
        img = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        def S(v):
            return int(round(v * scale))

        # ━━ 不透明底板 — 磨砂玻璃 frosted-glass 质感 ━━
        bg_fill = (207, 208, 197, 245)
        body = [
            (S(0), S(2)), (S(W - 8), S(0)),
            (S(W), S(4)), (S(W), S(H)),
            (S(8), S(H)), (S(0), S(H - 3)),
        ]
        draw.polygon(body, fill=bg_fill)

        # 内侧淡边框
        draw.polygon(body, outline=(180, 182, 170, 140))

        # 微妙水平纹理 (每隔几行画一条极淡的线, 模拟 HUD 行扫描)
        for yy in range(0, sh, S(3)):
            draw.line([(S(1), yy), (sw - S(1), yy)], fill=(255, 255, 255, 18))

        # SAO 角标装饰
        clen = S(7)
        cyan_a = (104, 228, 255, 140)
        gold_a = (212, 156, 23, 120)
        draw.line([(S(1), S(3)), (S(1) + clen, S(3))], fill=cyan_a, width=max(1, scale))
        draw.line([(S(1), S(3)), (S(1), S(3) + clen)], fill=cyan_a, width=max(1, scale))
        draw.line([(sw - S(1), sh - S(2)), (sw - S(1) - clen, sh - S(2))], fill=gold_a, width=max(1, scale))
        draw.line([(sw - S(1), sh - S(2)), (sw - S(1), sh - S(2) - clen)], fill=gold_a, width=max(1, scale))

        sta_cur, sta_max = self._sta_sta

        try:
            fn_sys = _get_hp_pil_font(9, 'sao')
            fn_label = _get_hp_pil_font(12, 'sao')
            fn_text = _get_hp_pil_font(10, 'sao')
        except Exception:
            fn_sys = fn_label = fn_text = None

        # ━━ 上行: SAO 系统状态行 ━━
        y_top = S(14)
        pad_x = S(10)

        # 左: ▪ SYSTEM [ LINK ACTIVE ]
        dot_r = S(2)
        draw.rectangle([pad_x, y_top - dot_r, pad_x + dot_r * 2, y_top + dot_r],
                        fill=(88, 152, 190, 200))
        draw.text((pad_x + S(7), y_top), 'SYSTEM',
                  fill=(97, 98, 86, 180), font=fn_sys, anchor='lm')
        draw.text((pad_x + S(52), y_top), '[ LINK ACTIVE ]',
                  fill=(97, 98, 86, 100), font=fn_sys, anchor='lm')

        # 中间: NErVGEAR
        draw.text((sw // 2, y_top), 'NErVGEAR',
                  fill=(97, 98, 86, 70), font=fn_sys, anchor='mm')

        # 右: 时间
        import time as _time
        _ts = _time.strftime('%H:%M:%S')
        draw.text((sw - pad_x, y_top), _ts,
                  fill=(97, 98, 86, 120), font=fn_text, anchor='rm')

        # 分隔线
        mid_y = S(28)
        draw.line([(S(6), mid_y), (sw - S(6), mid_y)], fill=(180, 182, 170, 80), width=max(1, scale))

        # ━━ 下行: STA 条 ━━
        y_sta = S(48)
        label_w = S(32)
        text_w = S(72)
        sta_pct = sta_cur / max(1, sta_max)

        # pip 指示点
        draw.rectangle([pad_x, y_sta - dot_r, pad_x + dot_r * 2, y_sta + dot_r],
                        fill=(212, 156, 23, 220))
        # STA 标签
        draw.text((pad_x + S(7), y_sta), 'STA',
                  fill=(212, 156, 23, 240), font=fn_label, anchor='lm')

        bar_x = pad_x + label_w + S(14)
        bar_w = sw - bar_x - text_w - S(8)
        bar_h = S(7)
        by_sta = y_sta - bar_h // 2

        # 条形背景 — 浅色磨砂轨道
        draw.rectangle([bar_x, by_sta, bar_x + bar_w, by_sta + bar_h],
                        fill=(190, 192, 180, 200), outline=(180, 182, 170, 100))
        # 填充
        sta_fill_w = max(0, int(bar_w * sta_pct))
        if sta_fill_w > 0:
            draw.rectangle([bar_x + scale, by_sta + scale, bar_x + sta_fill_w, by_sta + bar_h - scale],
                            fill=(243, 175, 18, 255))
            # 亮边高光
            draw.line([(bar_x + scale, by_sta + scale), (bar_x + sta_fill_w, by_sta + scale)],
                      fill=(255, 220, 100, 100), width=max(1, scale))
        # 数值
        draw.text((bar_x + bar_w + S(6), y_sta), f'{sta_cur}/{sta_max}',
                  fill=(97, 98, 86, 180), font=fn_text, anchor='lm')

        return img.resize((W, H), Image.LANCZOS)

    def _refresh_stamina_layered(self):
        """重新渲染并更新体力覆盖板分层窗口."""
        if self._destroyed or not self._stamina_hwnd:
            return
        try:
            if not self._stamina_win or not self._stamina_win.winfo_exists():
                return
        except Exception:
            return
        try:
            img = self._render_stamina_frame()
            dst_pos = None
            try:
                dst_pos = (self._stamina_win.winfo_rootx(), self._stamina_win.winfo_rooty())
            except Exception:
                pass
            _update_layered_win(self._stamina_hwnd, img, 255, dst_pos=dst_pos)
        except Exception:
            pass

    def _show_stamina_overlay(self):
        """显示体力覆盖板 (link-start 完成后调用)."""
        try:
            if self._stamina_win:
                self._stamina_win.deiconify()
                self._stamina_win.lift()
                if self._stamina_hwnd:
                    GWL_EXSTYLE = -20
                    WS_EX_LAYERED = 0x00080000
                    h = self._stamina_hwnd
                    ex = _user32.GetWindowLongW(ctypes.c_void_p(h), GWL_EXSTYLE)
                    if not (ex & WS_EX_LAYERED):
                        _user32.SetWindowLongW(ctypes.c_void_p(h), GWL_EXSTYLE, ex | WS_EX_LAYERED)
                self._refresh_stamina_layered()
        except Exception as e:
            print(f'[SAO-STA] show error: {e}')

    # ══════════════════════════════════════════════
    #  识别引擎
    # ══════════════════════════════════════════════
    def _stop_recognition_engines(self):
        """停止所有识别/数据引擎."""
        if getattr(self, '_auto_key_engine', None):
            try: self._auto_key_engine.stop()
            except Exception: pass
            self._auto_key_engine = None
        if getattr(self, '_boss_raid_engine', None):
            try: self._boss_raid_engine.stop()
            except Exception: pass
            self._boss_raid_engine = None
        engines = list(getattr(self, '_recognition_engines', []) or [])
        if not engines and self._recognition_engine:
            engines = [self._recognition_engine]
        for engine in engines:
            try: engine.stop()
            except Exception: pass
        self._recognition_engines = []
        self._recognition_engine = None
        self._packet_engine = None
        self._vision_engine = None

    def _reconfigure_data_engines(self):
        """重启 packet/vision 引擎以匹配当前数据源配置."""
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return
        self._stop_recognition_engines()

        engines = []
        try:
            from packet_bridge import PacketBridge
            packet_engine = PacketBridge(self._state_mgr, self._cfg_settings_ref,
                                         on_damage=self._on_packet_damage,
                                         on_monster_update=self._on_monster_update,
                                         on_boss_event=self._on_boss_event)
            packet_engine.start()
            engines.append(packet_engine)
            self._packet_engine = packet_engine
            print('[SAO Entity] Packet bridge started (network capture)')
        except Exception as e:
            import traceback
            print(f'[SAO Entity] Packet bridge FAILED: {e}')
            traceback.print_exc()
            self._packet_engine = None

        # DPS Tracker
        try:
            self._dps_tracker = DpsTracker()
            print('[SAO Entity] DPS tracker initialized')
        except Exception as e:
            print(f'[SAO Entity] DPS tracker init failed: {e}')
            self._dps_tracker = None

        try:
            from recognition import RecognitionEngine
            vision_engine = RecognitionEngine(self._state_mgr, self._cfg_settings_ref)
            vision_engine.start()
            engines.append(vision_engine)
            self._vision_engine = vision_engine
            print('[SAO Entity] Recognition engine started (window vision)')
        except Exception as e:
            import traceback
            print(f'[SAO Entity] Recognition engine FAILED: {e}')
            traceback.print_exc()
            self._vision_engine = None

        self._recognition_engines = engines
        self._recognition_engine = engines[0] if engines else None
        self._recognition_active = bool(engines)

    def _send_linked_key(self, key: str, press_mode: str = "tap",
                         hold_ms: int = 80, press_count: int = 1):
        """发送联动按键 (Boss→AutoKey linkage)."""
        try:
            from auto_key_engine import VK_NAME_MAP, INPUT, KEYBDINPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP
            import ctypes as _ct
            key = (key or "").strip().upper()
            vk = VK_NAME_MAP.get(key)
            if vk is None and len(key) == 1 and key.isalpha():
                vk = ord(key)
            if vk is None:
                return
            hold_s = max(0.015, hold_ms / 1000.0) if press_mode == "hold" else 0.015
            extra = _ct.c_ulong(0)
            for _ in range(max(1, press_count)):
                ki = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=0, time=0,
                                dwExtraInfo=_ct.pointer(extra))
                ev = INPUT(type=INPUT_KEYBOARD, ki=ki)
                _ct.windll.user32.SendInput(1, _ct.byref(ev), _ct.sizeof(INPUT))
                time.sleep(hold_s)
                ki2 = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=KEYEVENTF_KEYUP, time=0,
                                 dwExtraInfo=_ct.pointer(extra))
                ev2 = INPUT(type=INPUT_KEYBOARD, ki=ki2)
                _ct.windll.user32.SendInput(1, _ct.byref(ev2), _ct.sizeof(INPUT))
                if press_count > 1:
                    time.sleep(0.04)
        except Exception as e:
            print(f"[Linkage] send_key error: {e}")

    def _on_packet_damage(self, event):
        """Damage event callback from packet_parser → boss raid engine + DPS tracker."""
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_damage_event(event)
            except Exception: pass
        if self._dps_tracker:
            try: self._dps_tracker.on_damage_event(event)
            except Exception: pass

    def _on_monster_update(self, monster_data):
        """Monster update from packet_parser → boss raid engine."""
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_monster_update(monster_data)
            except Exception: pass

    def _on_boss_event(self, event):
        """Boss buff/event callback from packet_parser → boss raid engine."""
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_boss_event(event)
            except Exception: pass

    def _start_recognition(self):
        """启动游戏数据引擎 (抓包 + 纯识图 + AutoKey + BossRaid)."""
        try:
            from game_state import GameStateManager
            from config import SettingsManager as CfgSettings

            self._state_mgr = GameStateManager()
            cfg_settings = CfgSettings()

            # 加载上次缓存的游戏状态 (立即显示)
            self._state_mgr.load_cache(cfg_settings)
            self._cfg_settings_ref = cfg_settings

            # Restore sound settings
            try:
                from sao_sound import set_sound_enabled, set_sound_volume
                _snd_on = cfg_settings.get('sound_enabled', True)
                _snd_vol = cfg_settings.get('sound_volume', 70)
                set_sound_enabled(bool(_snd_on) if _snd_on is not None else True)
                set_sound_volume(int(_snd_vol) if _snd_vol is not None else 70)
            except Exception:
                pass

            # 用缓存名替换默认 "Player"
            cached_name = self._state_mgr.state.player_name
            if cached_name:
                self._username = cached_name
                disp = cached_name
                if len(disp) > 10:
                    disp = disp[:9] + '…'
                self._hp_display_name = disp
                print(f'[SAO Entity] 从缓存加载角色名: {cached_name}')

            self._reconfigure_data_engines()

            # AutoKey Engine
            self._auto_key_engine = AutoKeyEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                extra_gate=lambda: bool(getattr(self, '_recognition_active', False)),
            )
            self._auto_key_engine.start()

            # Boss Raid Engine + AutoKey Linkage
            self._boss_autokey_linkage = BossAutoKeyLinkage(
                self._cfg_settings_ref,
                send_key=self._send_linked_key,
                on_log=lambda msg: print(msg),
            )

            def _on_boss_alert_with_linkage(title, message):
                print(f'[SAO Entity] Boss Alert: {title} — {message}')
                if self._alert_overlay:
                    try: self._alert_overlay.show_alert(title, message)
                    except Exception: pass
                if self._boss_autokey_linkage:
                    try:
                        self._boss_autokey_linkage.on_boss_raid_alert(title, message)
                    except Exception:
                        pass

            self._boss_raid_engine = BossRaidEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                on_alert=_on_boss_alert_with_linkage,
                on_sound=lambda name: play_sound(name),
            )

            # ── 初始化 ULW 覆盖层 ──
            self._dps_visible = bool(self._get_setting('dps_enabled', True))
            try:
                self._dps_overlay = DpsOverlay(self.root, self._cfg_settings_ref)
                if self._dps_visible:
                    self._dps_overlay.show()
                print('[SAO Entity] DPS overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] DPS overlay init failed: {e}')
                self._dps_overlay = None

            try:
                self._boss_hp_overlay = BossHpOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Boss HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Boss HP overlay init failed: {e}')
                self._boss_hp_overlay = None

            try:
                self._alert_overlay = AlertOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Alert overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Alert overlay init failed: {e}')
                self._alert_overlay = None

            # 启动定时缓存保存 (每30秒)
            import threading as _thr
            def _cache_loop():
                import time as _t
                while True:
                    _t.sleep(30)
                    try:
                        self._state_mgr.save_cache(self._cfg_settings_ref)
                    except Exception:
                        pass
            _thr.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()

        except Exception as e:
            print(f'[SAO Entity] Data engine failed: {e}')
            import traceback; traceback.print_exc()
            self._recognition_active = False

    def _recognition_loop(self):
        """后台识别循环 — 读取 GameStateManager 并更新 HP 条 + 体力覆盖板 + DPS + Boss."""
        if self._destroyed:
            return
        if self._recognition_active and self._state_mgr:
            try:
                gs = self._state_mgr.state
                if gs.recognition_ok:
                    # HP data
                    if gs.hp_max > 0:
                        hp, hp_max = gs.hp_current, gs.hp_max
                    elif gs.hp_pct > 0:
                        hp, hp_max = int(gs.hp_pct * 100), 100
                    else:
                        hp, hp_max = 0, 1
                    if gs.stamina_max > 0:
                        sta, sta_max = gs.stamina_current, gs.stamina_max
                    elif gs.stamina_pct > 0:
                        sta, sta_max = int(gs.stamina_pct * 100), 100
                    else:
                        sta, sta_max = 0, 1
                    pct = hp / hp_max if hp_max > 0 else 0
                    self._float_progress_pct = pct
                    self._sta_hp = (hp, hp_max)
                    self._sta_sta = (sta, sta_max)
                    self._game_state = gs

                    # ── DPS tracker: 更新玩家信息 ──
                    if self._dps_tracker and gs.player_id:
                        try:
                            _p_uid = int(gs.player_id) if str(gs.player_id).isdigit() else 0
                            if _p_uid:
                                self._dps_tracker.set_self_uid(_p_uid)
                                self._dps_tracker.update_player_info(
                                    _p_uid,
                                    gs.player_name or '',
                                    gs.profession_name or '',
                                )
                        except Exception:
                            pass

                    # ── DPS Overlay push ──
                    if self._dps_tracker and self._dps_overlay and self._dps_visible:
                        try:
                            if self._dps_tracker.is_dirty():
                                _dps_snap = self._dps_tracker.get_snapshot()
                                self._dps_overlay.update(_dps_snap)
                            # DPS fade-out on idle
                            _dps_idle = self._dps_tracker.idle_seconds
                            _dps_fade_timeout = float(self._get_setting('dps_fade_timeout_s', 8.0) or 8.0)
                            if _dps_fade_timeout > 0 and _dps_idle >= _dps_fade_timeout and not self._dps_faded:
                                self._dps_overlay.fade_out()
                                self._dps_faded = True
                            elif self._dps_faded and _dps_idle < _dps_fade_timeout:
                                self._dps_overlay.fade_in()
                                self._dps_faded = False
                        except Exception:
                            pass

                    # ── Boss HP Overlay push ──
                    if self._boss_hp_overlay:
                        try:
                            _bb_raid_active = getattr(gs, 'boss_raid_active', False)
                            _bb_mode = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
                            _bb_src = getattr(gs, 'boss_hp_source', 'none') or 'none'
                            # Determine visibility
                            if _bb_mode == 'off':
                                _bb_show = False
                            elif _bb_mode == 'always':
                                _bb_show = (_bb_src != 'none') or _bb_raid_active
                            else:
                                _bb_show = _bb_raid_active
                            _bb_data = {
                                'active': _bb_show,
                                'hp_pct': round(getattr(gs, 'boss_hp_est_pct', 1.0), 3),
                                'hp_source': _bb_src,
                                'current_hp': getattr(gs, 'boss_current_hp', 0),
                                'total_hp': getattr(gs, 'boss_total_hp', 0),
                                'shield_active': getattr(gs, 'boss_shield_active', False),
                                'shield_pct': round(getattr(gs, 'boss_shield_pct', 0.0), 3),
                                'breaking_stage': getattr(gs, 'boss_breaking_stage', 0),
                                'extinction_pct': round(getattr(gs, 'boss_extinction_pct', 0.0), 3),
                                'in_overdrive': getattr(gs, 'boss_in_overdrive', False),
                                'invincible': getattr(gs, 'boss_invincible', False),
                                'boss_name': '',
                            }
                            self._boss_hp_overlay.update(_bb_data)
                        except Exception:
                            pass

                    # ── 同步玩家信息到显示变量 ──
                    if gs.player_name and gs.player_name != self._last_gs_name:
                        self._last_gs_name = gs.player_name
                        disp = gs.player_name
                        if len(disp) > 10:
                            disp = disp[:9] + '…'
                        self._hp_display_name = disp
                        self._username = gs.player_name
                    # ── 首次获取完整角色数据时自动保存 ──
                    if not self._profile_auto_saved and gs.player_name:
                        self._profile_auto_saved = True
                        try:
                            from character_profile import save_profile
                            lv = gs.level_extra if gs.level_extra > 0 else gs.level_base
                            save_profile(
                                username=gs.player_name,
                                profession=gs.profession_name or '',
                                level=lv if lv > 0 else 1,
                                uid=gs.player_id or '',
                            )
                            print(f'[SAO-UI] 自动保存角色: {gs.player_name}, '
                                  f'职业={gs.profession_name}, LV={lv}, UID={gs.player_id}')
                        except Exception:
                            pass
            except Exception as e:
                print(f'[SAO-UI] recognition loop error: {e}')
        if not self._destroyed:
            try:
                self.root.after(200, self._recognition_loop)
            except Exception:
                pass


    # ──────── 浮动呼吸动画 ────────
    def _start_float_breath(self):
        """idle 状态下轻微上下浮动 (模仿 SAO 菜单呼吸动画)"""
        if self._breath_active:
            return
        self._breath_active = True
        try:
            self._float.update_idletasks()
            self._breath_base_x = self._float.winfo_x()
            self._breath_base_y = self._float.winfo_y()
        except Exception:
            pass
        self._breath_t0 = time.time()
        self._breath_step()

    def _breath_step(self):
        if self._destroyed or not self._breath_active:
            return
        try:
            t = time.time() - self._breath_t0
            new_dx = int(round(math.sin(t * 1.25) * 3.0))
            new_dy = int(round(math.sin(t * 2.1) * 2.0))
            fx = self._breath_base_x + new_dx
            fy = self._breath_base_y + new_dy
            if self._float and self._float.winfo_exists():
                self._float.geometry(f'+{fx}+{fy}')
            self.root.after(16, self._breath_step)
        except Exception:
            pass

    def _stop_float_breath(self):
        self._breath_active = False
        try:
            if self._float and self._float.winfo_exists():
                self._float.geometry(f'+{self._breath_base_x}+{self._breath_base_y}')
        except Exception:
            pass

    def _attach_panel_float(self, panel, phase: float = 0.0, amp: float = 2.5):
        """给浮动面板附加轻微漂浮动画，且不再叠加额外 HUD 小条。"""
        t0 = time.time()

        def _step():
            if self._destroyed:
                return
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return

            now = time.time() - t0
            new_dx = int(amp * math.sin(now * 0.82 + phase))
            new_dy = int(amp * math.sin(now * 0.61 + phase + 1.2))
            old_dx = getattr(panel, '_fdx', 0)
            old_dy = getattr(panel, '_fdy', 0)
            dd_x, dd_y = new_dx - old_dx, new_dy - old_dy
            if dd_x != 0 or dd_y != 0:
                try:
                    cx = panel.winfo_x()
                    cy = panel.winfo_y()
                    panel.geometry(f'+{cx + dd_x}+{cy + dd_y}')
                except Exception:
                    pass
            panel._fdx = new_dx
            panel._fdy = new_dy
            try:
                self.root.after(16, _step)
            except Exception:
                pass

        panel._fdx = 0
        panel._fdy = 0
        _step()

    def _float_click(self, e):
        self._drag['x'] = e.x_root
        self._drag['y'] = e.y_root
        self._drag['dragging'] = False
        self._stop_float_breath()

    def _float_drag(self, e):
        dx = abs(e.x_root - self._drag['x'])
        dy = abs(e.y_root - self._drag['y'])
        if dx > 5 or dy > 5:
            self._drag['dragging'] = True
        if self._drag['dragging']:
            mx = e.x_root - self._fw // 2
            my = e.y_root - self._fh // 2
            self._float.geometry(f'+{mx}+{my}')

    def _float_release(self, e):
        if self._skip_canvas_click:
            self._skip_canvas_click = False
            return
        if self._drag['dragging']:
            try:
                self._breath_base_x = self._float.winfo_x()
                self._breath_base_y = self._float.winfo_y()
                self.settings.set('float_x', self._breath_base_x)
                self.settings.set('float_y', self._breath_base_y)
                self.settings.save()
            except Exception:
                pass
            self._breath_t0 = time.time()
            self._breath_active = True
            self._breath_step()
        else:
            self._toggle_sao_menu()

    def _float_enter(self, e):
        """高亮悬浮 HP 组件"""
        try:
            self._hp_hover = True
            self._float_alpha = 1.0
            self._refresh_hp_layered()
        except Exception:
            pass

    def _float_leave(self, e):
        """恢复默认色"""
        try:
            self._hp_hover = False
            self._float_alpha = 1.0   # 完全不透明 — 覆盖游戏原生条
            self._refresh_hp_layered()
        except Exception:
            pass

    def _update_float_display(self):
        """更新悬浮 HP 组件 — 由 _render_hp_dynamic 统一处理，仅触发刷新。"""
        self._refresh_hp_layered()

    def _update_float_status(self):
        self._update_float_display()

    def _update_float_fname(self, name=''):
        """HP 组件风格: 无文件名显示, 保留接口兼容"""
        pass

    def _animate_float_to(self, x0, y0, x1, y1, ms=700):
        """将悬浮窗口从 (x0,y0) 平滑动画到 (x1,y1)"""
        steps = max(1, ms // 16)
        step = [0]
        def tick():
            if self._destroyed:
                return
            if not self._float.winfo_exists():
                return
            step[0] += 1
            t = min(1.0, step[0] / steps)
            et = ease_out(t)
            x = int(x0 + (x1 - x0) * et)
            y = int(y0 + (y1 - y0) * et)
            self._float.geometry(f'+{x}+{y}')
            try:
                self._refresh_hp_layered()
            except Exception:
                pass
            if t < 1.0:
                try:
                    self.root.after(16, tick)
                except Exception:
                    pass
        tick()

    def _lift_float_loop(self):
        """SAO 菜单开启时持续将悬浮按钮保持在最上层"""
        if self._destroyed or not self._lift_loop_active:
            return
        try:
            if self._float.winfo_exists():
                self._float.lift()
        except Exception:
            pass
        try:
            self.root.after(150, self._lift_float_loop)
        except Exception:
            pass

    # ══════════════════════════════════════════════
    #  SAO 菜单 = 主界面
    # ══════════════════════════════════════════════
    def _make_player_panel(self, parent):
        """工厂: 为 SAO 菜单创建左侧信息面板"""
        panel = SAOPlayerPanel(parent,
                               username=self._username or 'Player',
                               profession=self._profession or '')
        self._player_panel = panel

        # 等级信息
        panel._level = self._level
        try:
            lv, cur_xp, need_xp = calc_level(self._xp)
            panel._xp_percent = cur_xp / max(1, need_xp)
            panel._xp_total = self._xp
        except Exception:
            panel._xp_percent = 0.0
            panel._xp_total = 0

        return panel

    def _build_menu_children(self):
        """动态构建子菜单 (支持状态反映) — SAO Auto (5 categories)"""
        # 读取快捷键配置
        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS)
        def _k(key_id):
            v = hk.get(key_id, DEFAULT_HOTKEYS.get(key_id, ''))
            return f'  [{v}]' if v else ''

        recog_label = '识别: ON' if getattr(self, '_recognition_active', False) else '识别: OFF'
        topmost_label = '置顶: ON' if self._float.attributes('-topmost') else '置顶: OFF'

        # AutoKey 状态
        ak_config = self._load_auto_key_config() if self._cfg_settings_ref else {}
        ak_on = bool(ak_config.get('enabled', False))
        ak_label = f'AutoKey: {"ON" if ak_on else "OFF"}' + _k('toggle_auto_script')

        # BossRaid 状态
        br_config = self._load_boss_raid_config() if self._cfg_settings_ref else {}
        br_on = bool(br_config.get('enabled', False))
        br_label = f'BossRaid: {"ON" if br_on else "OFF"}' + _k('boss_raid_start')

        # Sound / Display 状态
        _snd_on = bool(self._get_setting('sound_enabled', True))
        _dps_on = bool(self._get_setting('dps_enabled', True))
        _burst_on = bool(self._get_setting('burst_enabled', True))
        _bbm = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        _bbm_labels = {'always': '常显', 'boss_raid': 'Boss战', 'off': '关闭'}
        _bbm_disp = _bbm_labels.get(_bbm, _bbm)

        return {
            '控制': [
                {'icon': '⚙', 'label': recog_label + _k('toggle_recognition'), 'command': self._toggle_recognition_menu},
                {'icon': '⬆', 'label': topmost_label + _k('toggle_topmost'), 'command': self._toggle_topmost},
                {'icon': '✓', 'label': '保存设置', 'command': lambda: self.settings.save()},
            ],
            '自动': [
                {'icon': '⚡', 'label': ak_label, 'command': self._toggle_auto_script},
                {'icon': '◆', 'label': 'AutoKey 配置', 'command': self._toggle_autokey_panel},
            ],
            'Boss': [
                {'icon': '⚔', 'label': br_label, 'command': self._toggle_boss_raid},
                {'icon': '▸', 'label': '下一阶段' + _k('boss_raid_next_phase'), 'command': self._boss_raid_next_phase},
                {'icon': '◆', 'label': 'BossRaid 配置', 'command': self._toggle_bossraid_panel},
            ],
            '面板': [
                {'icon': '◉', 'label': '状态面板', 'command': self._toggle_status_panel},
                {'icon': '◈', 'label': '一键隐藏面板' + (' ✓' if self._panels_hidden else ''), 'command': self._toggle_hide_all_panels},
                {'icon': '─', 'label': '──────────'},  # separator
                {'icon': '♪', 'label': f'音效: {"ON" if _snd_on else "OFF"}', 'command': self._toggle_sound_enabled},
                {'icon': '♪', 'label': '音量+', 'command': lambda: self._adj_sound_volume(10)},
                {'icon': '♪', 'label': '音量-', 'command': lambda: self._adj_sound_volume(-10)},
                {'icon': '◆', 'label': f'DPS面板: {"ON" if _dps_on else "OFF"}', 'command': self._toggle_dps_enabled},
                {'icon': '◆', 'label': f'爆发提示: {"ON" if _burst_on else "OFF"}', 'command': self._toggle_burst_enabled},
                {'icon': '◇', 'label': f'Boss血条: {_bbm_disp}', 'command': self._cycle_boss_bar_mode},
            ],
            '关于': [
                {'icon': '◇', 'label': '关于本程序', 'command': self._show_about},
                {'icon': '✎', 'label': '修改角色资料', 'command': self._edit_profile},
                {'icon': '◇', 'label': '切换到 WebView UI', 'command': self._switch_to_webview_ui},
                {'icon': '✕', 'label': '退出', 'command': self._on_close},
            ],
        }

    def _toggle_autokey_panel(self):
        """打开/关闭 AutoKey 配置面板 (tkinter)."""
        if not self._autokey_panel:
            self._autokey_panel = AutoKeyPanel(
                master=self.root,
                load_fn=self._load_auto_key_config,
                save_fn=self._save_auto_key_config,
                engine_ref=lambda: self._auto_key_engine,
                on_toggle=self._toggle_auto_script,
                author_fn=getattr(self, '_auto_key_author_snapshot', None),
            )
        self._autokey_panel.toggle()

    def _toggle_bossraid_panel(self):
        """打开/关闭 BossRaid 配置面板 (tkinter)."""
        if not self._bossraid_panel:
            self._bossraid_panel = BossRaidPanel(
                master=self.root,
                load_fn=self._load_boss_raid_config,
                save_fn=self._save_boss_raid_config,
                engine_ref=lambda: self._boss_raid_engine,
                on_toggle=self._toggle_boss_raid,
                on_start=self._toggle_boss_raid,
                on_next=self._boss_raid_next_phase,
                on_reset=lambda: (
                    self._boss_raid_engine.reset() if self._boss_raid_engine else None
                ),
            )
        self._bossraid_panel.toggle()

    def _setup_sao_menu(self):
        """构建 SAO PopUpMenu 菜单 = 主界面 (5 categories)"""
        self._menu_icons = [
            {'name': '控制', 'icon': '⚙', 'can_active': True},
            {'name': '自动', 'icon': '⚡', 'can_active': True},
            {'name': 'Boss', 'icon': '⚔', 'can_active': True},
            {'name': '面板', 'icon': '◆', 'can_active': True},
            {'name': '关于', 'icon': 'ℹ', 'can_active': True},
        ]

        self._sao_menu = SAOPopUpMenu(
            self.root, self._menu_icons, self._build_menu_children(),
            username=self._username or 'Player',
            description=self._profession or 'SAO Auto — 游戏辅助 UI',
            on_close=self._on_sao_menu_close,
            on_open=self._on_sao_menu_open,
            key_code='a',
            slide_down=False,
            left_widget_factory=self._make_player_panel,
            anchor_widget=self._float,
        )
        self._sao_menu.bind_events()

    def _fade_panel_in(self, panel, target=0.92, duration_ms=350):
        """浮动面板淡入 — 平滑 ease-out 动画, 并确保鱼眼叠加层运行"""
        # 面板打开时, 如果鱼眼尚未启动则启动
        if self._fisheye_ov is None:
            try:
                self.root.after(100, self._start_fisheye_overlay)
            except Exception:
                pass

        t0 = time.time()
        dur = duration_ms / 1000.0

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / dur)
            et = 1 - (1 - t) ** 3  # ease_out
            panel.attributes('-alpha', target * et)
            if t < 1.0:
                try:
                    self.root.after(16, _step)
                except Exception:
                    pass

        _step()

    def _fade_panel_out(self, panel, attr_name, settings_key, duration_ms=220):
        """浮动面板淡出 — ease-in 动画, 完成后 destroy"""
        try: play_sound('alert_close')
        except: pass
        try:
            start_alpha = float(panel.attributes('-alpha'))
        except Exception:
            start_alpha = 0.92

        t0 = time.time()
        dur = duration_ms / 1000.0

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / dur)
            et = t * t  # ease_in
            panel.attributes('-alpha', start_alpha * (1.0 - et))
            if t < 1.0:
                try:
                    self.root.after(16, _step)
                except Exception:
                    pass
            else:
                try:
                    panel.destroy()
                except Exception:
                    pass
                setattr(self, attr_name, None)
                self.settings.set(settings_key, False)
                self.settings.save()
                self._maybe_stop_fisheye()

        _step()

    def _toggle_sao_menu(self):
        if self._sao_menu.visible:
            try:
                play_sound('menu_close')
            except Exception:
                pass
            self._play_motion_blur(closing=True)
            self._sao_menu.close()
        else:
            try:
                play_sound('menu_open')
            except Exception:
                pass
            self._play_motion_blur(closing=False)
            self._sao_menu.child_menus = self._build_menu_children()
            self._sao_menu.open()
            # 立即将悬浮按钮浮到 overlay 之上 (避免撕裂)
            self._float.lift()

    def _on_sao_menu_open(self):
        """SAO 菜单打开时 — 停止呼吸, 启动持久鱼眼 (Win32 z-order 接管)"""
        self._stop_float_breath()
        # 不启动 _lift_float_loop (tkinter .lift() 会引起闪烁);
        # z-order 完全由 _start_fisheye_overlay 内的 Win32 SetWindowPos 管理
        self._lift_loop_active = False
        # 延迟启动鱼眼叠加 (等菜单渲染完再截图), 带重试确保首次也能生效
        self._start_fisheye_with_retry(retries=5, delay=80)

    def _start_fisheye_with_retry(self, retries=5, delay=80):
        """带重试的鱼眼启动 — 首次进入时菜单可能还未完成渲染"""
        if self._destroyed:
            return
        if self._fisheye_ov is not None:
            return  # 已在运行
        if retries <= 0:
            return
        if self._sao_menu.visible or self._any_panel_open():
            self._start_fisheye_overlay()
        else:
            try:
                self.root.after(delay, lambda: self._start_fisheye_with_retry(retries - 1, delay))
            except Exception:
                pass

    def _any_panel_open(self):
        """检查是否有任何浮动面板处于打开且可见状态"""
        if self._panels_hidden:
            return False
        for p in (self._piano_panel, self._viz_panel,
                  self._status_panel, self._control_panel):
            try:
                if p and p.winfo_exists():
                    return True
            except Exception:
                pass
        return False

    def _maybe_stop_fisheye(self):
        """仅当 SAO 菜单和所有面板都关闭时才销毁鱼眼叠加层"""
        if self._sao_menu.visible:
            return
        if self._any_panel_open():
            return
        self._stop_fisheye_overlay()

    def _on_sao_menu_close(self):
        """SAO 菜单关闭时 — 重启呼吸动画; 面板仍开时保持鱼眼, 否则渐隐销毁"""
        self._lift_loop_active = False
        self._player_panel = None
        self._maybe_stop_fisheye()
        if not self._destroyed:
            pass  # 呼吸动画已禁用 (固定位置)

    def _refresh_menu_if_open(self):
        """如果菜单打开, 刷新子菜单和面板"""
        if self._sao_menu.visible:
            children = self._build_menu_children()
            for name, items in children.items():
                self._sao_menu.refresh_child_menu(name, items)
        self._update_float_status()

    # ══════════════════════════════════════════════
    #  浮动面板: 钢琴 / 可视化
    # ══════════════════════════════════════════════
    def _toggle_piano_panel(self):
        """钢琴面板已禁用 (SAO Auto)"""
        pass
    def _toggle_status_panel(self):
        """浮动状态面板 — 显示识别状态 + 引擎信息"""
        if self._status_panel and self._status_panel.winfo_exists():
            self._fade_panel_out(self._status_panel, '_status_panel', 'show_status')
            return

        try: play_sound('panel')
        except: pass
        sw, sh = 220, 130
        saved_sx = self.settings.get('status_x', None)
        saved_sy = self.settings.get('status_y', None)
        if saved_sx is not None:
            fx, fy = int(saved_sx), int(saved_sy)
        else:
            fx = self._float.winfo_x() + self._fw + 10
            fy = self._float.winfo_y()
            if fx + sw > self._float.winfo_screenwidth() - 10:
                fx = self._float.winfo_x() - sw - 10

        self._status_panel = tk.Toplevel(self.root)
        self._status_panel.overrideredirect(True)
        self._status_panel.attributes('-topmost', True)
        self._status_panel.attributes('-alpha', 0.0)
        self._status_panel.geometry(f'{sw}x{sh}+{fx}+{fy}')
        self._status_panel.configure(bg=_SAO_PANEL_HEADER_BG)
        _apply_panel_style(self._status_panel)

        border = tk.Frame(self._status_panel, bg=_SAO_PANEL_BORDER, padx=1, pady=1)
        border.pack(fill=tk.BOTH, expand=True)
        inner = tk.Frame(border, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill=tk.BOTH, expand=True)

        hdr, close_lbl = _sao_panel_header(inner, '◉', 'STATUS', self._toggle_status_panel)

        body = _sao_panel_body(inner)
        body_pad = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        body_pad.pack(fill=tk.BOTH, expand=True, padx=10, pady=6)

        # 识别状态行
        recog_text = 'ON' if getattr(self, '_recognition_active', False) else 'OFF'
        recog_fg = '#3ad86c' if recog_text == 'ON' else '#556677'
        self._status_recog_lbl = _sao_row(body_pad, '识别', recog_text,
                                           value_fg=recog_fg,
                                           value_font=get_cjk_font(9, True))

        # 数据源行
        src_text = 'Packet'
        if getattr(self, '_cfg_settings_ref', None):
            src = self._cfg_settings_ref.get('data_source', 'packet')
            src_text = 'Packet' if src == 'packet' else 'OCR'
        self._status_source_lbl = _sao_row(body_pad, '数据源', src_text,
                                            value_fg=_SAO_PANEL_GOLD)

        # 底部 HUD 装饰
        hud_cv = _sao_panel_hud_canvas(body)
        hud_cv.create_text(4, 8, text='SYS:STATUS', anchor='w',
                           font=('Consolas', 6), fill='#d0d0d0')
        hud_cv.create_line(80, 8, sw - 10, 8, fill='#e8e8e8', width=1)

        # 拖拽
        _sd = {'x': 0, 'y': 0}
        def sdstart(e): _sd['x'], _sd['y'] = e.x_root, e.y_root
        def sdmove(e):
            dx, dy = e.x_root - _sd['x'], e.y_root - _sd['y']
            nx, ny = self._status_panel.winfo_x()+dx, self._status_panel.winfo_y()+dy
            self._status_panel.geometry(f'+{nx}+{ny}')
            _sd['x'], _sd['y'] = e.x_root, e.y_root
            self.settings.set('status_x', nx); self.settings.set('status_y', ny)
        _bind_panel_drag(hdr, close_lbl, sdstart, sdmove)

        self._fade_panel_in(self._status_panel, target=0.92)
        self._attach_sao_panel_fx(self._status_panel, hdr, inner)
        self._attach_panel_float(self._status_panel, phase=2.0)
        self._update_status_panel()
        self.settings.set('show_status', True)
        self.settings.save()

    def _update_status_panel(self):
        """刷新状态面板内容"""
        if not (self._status_panel and self._status_panel.winfo_exists()):
            return
        # 识别状态
        if hasattr(self, '_status_recog_lbl'):
            recog_text = 'ON' if getattr(self, '_recognition_active', False) else 'OFF'
            recog_fg = '#3ad86c' if recog_text == 'ON' else '#556677'
            self._status_recog_lbl.configure(text=recog_text, fg=recog_fg)

    def _toggle_viz_panel(self):
        """可视化面板已禁用 (SAO Auto)"""
        pass
    def _toggle_control_panel(self):
        """控制面板已禁用 (SAO Auto — MIDI已移除)"""
        pass

    def _toggle_hide_all_panels(self):
        """一键隐藏/显示所有浮动面板 (不销毁, 只是 withdraw/deiconify)"""
        panels = [
            ('piano',   self._piano_panel),
            ('viz',     self._viz_panel),
            ('status',  self._status_panel),
            ('control', self._control_panel),
        ]

        if not self._panels_hidden:
            # ── 隐藏 ──
            self._hidden_panels_snapshot = []
            for name, p in panels:
                try:
                    if p and p.winfo_exists():
                        self._hidden_panels_snapshot.append(name)
                        p.withdraw()
                except Exception:
                    pass
            self._panels_hidden = True
            self._maybe_stop_fisheye()
        else:
            # ── 恢复 ──
            for name, p in panels:
                try:
                    if name in self._hidden_panels_snapshot and p and p.winfo_exists():
                        p.deiconify()
                        p.lift()
                except Exception:
                    pass
            self._hidden_panels_snapshot = []
            self._panels_hidden = False
            # 面板恢复后确保鱼眼也恢复
            if self._fisheye_ov is None and self._any_panel_open():
                self.root.after(100, self._start_fisheye_overlay)

        self._refresh_menu_if_open()

    def _restore_panels(self):
        """恢复上次会话中打开的浮动面板"""
        # 如果面板处于隐藏状态则跳过恢复
        if self._panels_hidden:
            return
        if self.settings.get('show_piano', False):
            if not (self._piano_panel and self._piano_panel.winfo_exists()):
                self._toggle_piano_panel()
        if self.settings.get('show_viz', False):
            if not (self._viz_panel and self._viz_panel.winfo_exists()):
                self._toggle_viz_panel()
        if self.settings.get('show_status', False):
            if not (self._status_panel and self._status_panel.winfo_exists()):
                self._toggle_status_panel()
        if self.settings.get('show_control', False):
            if not (self._control_panel and self._control_panel.winfo_exists()):
                self._toggle_control_panel()

    # ══════════════════════════════════════════════════════════════
    #  点击悬浮按钮 → 径向运动模糊闪现
    # ══════════════════════════════════════════════════════════════
    def _play_motion_blur(self, closing=False):
        """
        悬浮按钮点击时的径向运动模糊效果 (SAO 菜单展开/收起).

        以悬浮按钮为中心, 截取屏幕 → 径向缩放模糊 → 叠加层渐隐.
        • 后台线程: 截屏 + 径向模糊
        • 主线程: 显示结果 + 渐隐动画
        • WDA_EXCLUDEFROMCAPTURE: 防止鱼眼层捕获到此叠加层 (消除撕裂)
        • BILINEAR 缩放: 减少锯齿/马赛克感
        """
        try:
            from PIL import ImageGrab, Image, ImageTk, ImageFilter
        except ImportError:
            return

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()

        # 悬浮按钮中心作为模糊焦点
        try:
            fx = self._float.winfo_x() + self._fw // 2
            fy = self._float.winfo_y() + self._fh // 2
        except Exception:
            fx, fy = sw // 2, sh // 2

        def _build_and_show():
            """后台: 截屏 + 径向模糊 → 主线程显示."""
            # 截屏
            shot = None
            try:
                import mss as _mss_mod
                _sct = _mss_mod.mss()
                _mon = {"top": 0, "left": 0, "width": sw, "height": sh}
                s = _sct.grab(_mon)
                shot = Image.frombytes('RGB', s.size, s.rgb)
            except Exception:
                pass
            if shot is None:
                for _g in (
                    lambda: ImageGrab.grab(bbox=(0, 0, sw, sh), all_screens=True),
                    lambda: ImageGrab.grab(bbox=(0, 0, sw, sh)),
                    lambda: ImageGrab.grab(),
                ):
                    try:
                        shot = _g()
                        break
                    except Exception:
                        continue
            if shot is None:
                return

            # 半分辨率处理 (1/2 而非 1/3, 提升清晰度)
            hw, hh = sw // 2, sh // 2
            small = shot.resize((hw, hh), Image.BILINEAR)
            cx, cy = fx / 2.0, fy / 2.0

            # 径向缩放模糊: 多次微缩放叠加
            import numpy as np
            acc = np.array(small, dtype=np.float32)
            n_layers = 5
            for i in range(1, n_layers + 1):
                scale = 1.0 + i * 0.012
                nw = int(hw * scale)
                nh = int(hh * scale)
                zoomed = small.resize((nw, nh), Image.BILINEAR)
                ox = int(cx * scale - cx)
                oy = int(cy * scale - cy)
                ox = max(0, min(ox, nw - hw))
                oy = max(0, min(oy, nh - hh))
                crop = zoomed.crop((ox, oy, ox + hw, oy + hh))
                acc += np.array(crop, dtype=np.float32)
            blurred = Image.fromarray(
                (acc / (n_layers + 1)).clip(0, 255).astype(np.uint8))

            from PIL import ImageEnhance
            if closing:
                blurred = ImageEnhance.Brightness(blurred).enhance(0.85)
            else:
                blurred = ImageEnhance.Brightness(blurred).enhance(1.12)

            full = blurred.resize((sw, sh), Image.BILINEAR)

            try:
                self.root.after(0, lambda img=full: _display(img))
            except Exception:
                pass

        def _display(pil_img):
            """主线程: 显示模糊图 + 350ms ease-out 渐隐."""
            try:
                mb_ov = tk.Toplevel(self.root)
                mb_ov.overrideredirect(True)
                mb_ov.attributes('-topmost', True)
                mb_ov.attributes('-alpha', 0.0)
                mb_ov.geometry(f'{sw}x{sh}+0+0')
                cv = tk.Canvas(mb_ov, width=sw, height=sh,
                               highlightthickness=0, bg='black')
                cv.pack(fill=tk.BOTH, expand=True)
                photo = ImageTk.PhotoImage(pil_img)
                cv.create_image(0, 0, image=photo, anchor='nw')
                cv._photo = photo

                # WDA_EXCLUDEFROMCAPTURE: 鱼眼截屏不会捕获到此 overlay
                try:
                    import ctypes as _ct
                    _u32 = _ct.windll.user32
                    mb_ov.update_idletasks()
                    hwnd = _u32.GetParent(mb_ov.winfo_id()) or mb_ov.winfo_id()
                    _u32.SetWindowDisplayAffinity(hwnd, 0x00000011)
                except Exception:
                    pass
            except Exception:
                return

            # 快速渐入 (50ms) → 缓慢渐隐 (350ms), 消除突然出现的闪烁感
            _t0 = time.time()
            _fadein_dur = 0.05
            _fadeout_dur = 0.35
            _peak = 0.72

            def _mblur_anim():
                dt = time.time() - _t0
                if dt < _fadein_dur:
                    # 渐入阶段
                    a = _peak * (dt / _fadein_dur)
                elif dt < _fadein_dur + _fadeout_dur:
                    # 渐隐阶段
                    t = (dt - _fadein_dur) / _fadeout_dur
                    a = _peak * (1.0 - t ** 0.6)
                else:
                    try: mb_ov.destroy()
                    except Exception: pass
                    return
                try: mb_ov.attributes('-alpha', max(0.0, a))
                except Exception: pass
                try: mb_ov.after(16, _mblur_anim)
                except Exception: pass

            mb_ov.after(1, _mblur_anim)

        import threading as _th
        _th.Thread(target=_build_and_show, daemon=True).start()

    # ══════════════════════════════════════════════════════════════
    #  持久鱼眼叠加层 (菜单开启时常驻, 关闭时销毁)
    # ══════════════════════════════════════════════════════════════
    def _start_fisheye_overlay(self):
        """
        SAO 菜单开启期间的持久鱼眼叠加层 (实时 60fps 双缓冲).

        架构:
          • 后台 _worker 线程: 截屏 + GPU/numpy 畸变 + 缩放 → _latest_frame
          • 主线程 16ms _tick 状态机: 从 _latest_frame 读 → PhotoImage → canvas
          • 保证 60fps 显示 (alpha 动画 + 内容), 捕获帧率按硬件自适应
          • _tick: init→fadein→active→fadeout→销毁
          • 菜单关闭 16ms 内检测 → 同步渐隐, 无需二次点击
        """
        self._stop_fisheye_overlay()
        if not self._sao_menu.visible and not self._any_panel_open():
            return
        self._lift_loop_active = False

        try:
            from PIL import ImageGrab, Image, ImageTk
        except ImportError:
            return

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        hw, hh = int(sw * 0.85), int(sh * 0.85)   # 85% 分辨率 (清晰度提升)

        # ── 创建叠加层窗口 (不设 topmost, 自然低于 topmost UI) ──
        # GPU/numpy 初始化已移至后台 _worker 线程 (消除主线程阻塞)
        try:
            ov = tk.Toplevel(self.root)
            ov.overrideredirect(True)
            # 不设 -topmost: overlay 位于所有 topmost 窗口下方
            ov.attributes('-alpha', 0.0)
            ov.geometry(f'{sw}x{sh}+0+0')
            cv_ov = tk.Canvas(ov, width=sw, height=sh,
                              highlightthickness=0, bg='black')
            cv_ov.pack(fill=tk.BOTH, expand=True)
            ov._cv = cv_ov
            ov._img_id = None
            self._fisheye_ov = ov
        except Exception:
            return

        # ── Win32 API (64-bit safe) ──
        _hwnd_ref = [0]
        try:
            import ctypes as _ct
            _u32 = _ct.windll.user32
            _vp = _ct.c_void_p
            _u32.GetParent.argtypes                 = [_vp]
            _u32.GetParent.restype                  = _vp
            _u32.GetWindowLongW.argtypes            = [_vp, _ct.c_int]
            _u32.GetWindowLongW.restype             = _ct.c_long
            _u32.SetWindowLongW.argtypes            = [_vp, _ct.c_int, _ct.c_long]
            _u32.SetWindowLongW.restype             = _ct.c_long
            _u32.SetLayeredWindowAttributes.argtypes = [_vp, _ct.c_uint,
                                                        _ct.c_ubyte, _ct.c_uint]
            _u32.SetLayeredWindowAttributes.restype  = _ct.c_int
            # SetWindowDisplayAffinity (Win10 2004+): 排除屏幕捕获
            _u32.SetWindowDisplayAffinity.argtypes  = [_vp, _ct.c_uint]
            _u32.SetWindowDisplayAffinity.restype   = _ct.c_int
        except Exception:
            _u32 = None

        _GWL_EXSTYLE       = -20
        _WS_EX_TRANSPARENT = 0x00000020
        _LWA_ALPHA         = 0x00000002
        _WDA_EXCLUDEFROMCAPTURE = 0x00000011

        def _set_alpha(bv):
            if _hwnd_ref[0] and _u32:
                try:
                    _u32.SetLayeredWindowAttributes(
                        _hwnd_ref[0], 0, bv, _LWA_ALPHA)
                except Exception:
                    pass

        def _init_layered():
            if not _u32:
                return
            try:
                ov.update_idletasks()
                hwnd = _u32.GetParent(ov.winfo_id()) or ov.winfo_id()
                _hwnd_ref[0] = hwnd
                cur = _u32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
                _u32.SetWindowLongW(hwnd, _GWL_EXSTYLE,
                                    cur | _WS_EX_TRANSPARENT)
                _u32.SetLayeredWindowAttributes(hwnd, 0, 0, _LWA_ALPHA)
                # WDA_EXCLUDEFROMCAPTURE: 叠加层对 ImageGrab 不可见
                try:
                    _u32.SetWindowDisplayAffinity(hwnd, _WDA_EXCLUDEFROMCAPTURE)
                except Exception:
                    pass
            except Exception:
                pass

        # ── 60fps 双缓冲状态机 ──
        _ALPHA_MAX = 255
        _alpha_cur = [0.0]
        _FADEIN_STEP = _ALPHA_MAX / 38.0     # 38 × 16ms ≈ 600ms 渐显
        _FADEOUT_STEP = _ALPHA_MAX / 25.0    # 25 × 16ms ≈ 400ms 渐隐
        _running = [True]
        _latest_frame = [None]   # 后台线程写, 主线程读 (GIL 原子)
        _state = ['init']        # init → fadein → active → fadeout → 销毁
        _last_shown = [None]     # 去重: 同帧不重建 PhotoImage

        def _show(frame):
            """仅在新帧到达时创建 PhotoImage (跳过重复帧)."""
            if frame is None or frame is _last_shown[0]:
                return
            _last_shown[0] = frame
            try:
                photo = ImageTk.PhotoImage(frame)
                if ov._img_id is None:
                    ov._img_id = cv_ov.create_image(
                        0, 0, image=photo, anchor='nw')
                else:
                    cv_ov.itemconfig(ov._img_id, image=photo)
                cv_ov._photo = photo
            except Exception:
                pass

        def _tick():
            """主线程 60fps 状态机: fadein / display / fadeout 全在此."""
            if self._fisheye_ov is None:
                return
            s = _state[0]
            if s == 'init':
                f = _latest_frame[0]
                if f is not None:
                    _show(f)
                    _state[0] = 'fadein'
            elif s == 'fadein':
                if not self._sao_menu.visible:
                    _state[0] = 'fadeout'
                    _running[0] = False
                else:
                    _alpha_cur[0] = min(_ALPHA_MAX,
                                        _alpha_cur[0] + _FADEIN_STEP)
                    _set_alpha(int(_alpha_cur[0]))
                    _show(_latest_frame[0])
                    if _alpha_cur[0] >= _ALPHA_MAX:
                        _state[0] = 'active'
            elif s == 'active':
                if not self._sao_menu.visible:
                    _state[0] = 'fadeout'
                    _running[0] = False
                else:
                    _show(_latest_frame[0])
            elif s == 'fadeout':
                _alpha_cur[0] = max(0, _alpha_cur[0] - _FADEOUT_STEP)
                _set_alpha(int(_alpha_cur[0]))
                if _alpha_cur[0] <= 0:
                    self._stop_fisheye_overlay()
                    return
            try: ov.after(16, _tick)
            except Exception: pass

        if self._destroyed:
            return
        ov.after(30, _init_layered)
        ov.after(50, _tick)

        # ── 后台 worker: 截屏 + 畸变 + 缩放 → _latest_frame ──
        def _worker():
            """后台线程: 全部重活在此, 主线程仅 PhotoImage."""
            import time as _time

            # ── 优先 mss 快速截屏 (DXGI, ~5ms), fallback ImageGrab (~30ms) ──
            _cap_fn = None
            try:
                import mss as _mss_mod
                _sct = _mss_mod.mss()
                _mon = {"top": 0, "left": 0, "width": sw, "height": sh}
                def _cap_mss():
                    s = _sct.grab(_mon)
                    return Image.frombytes('RGB', s.size, s.rgb)
                _cap_fn = _cap_mss
            except Exception:
                pass
            if _cap_fn is None:
                def _cap_ig():
                    for _g in (
                        lambda: ImageGrab.grab(bbox=(0, 0, sw, sh),
                                               all_screens=True),
                        lambda: ImageGrab.grab(bbox=(0, 0, sw, sh)),
                        lambda: ImageGrab.grab(),
                    ):
                        try: return _g()
                        except Exception: continue
                    return None
                _cap_fn = _cap_ig

            # ── GPU 初始化 (GL 上下文在本线程创建 & 使用, 线程安全) ──
            _gl_ok = False
            _ctx = _prog = _vbo = _vao = _tex = _fbo = None
            try:
                import moderngl
                _ctx = moderngl.create_standalone_context()
                _prog = _ctx.program(
                    vertex_shader='''
                        #version 330
                        in vec2 in_pos;
                        out vec2 uv;
                        void main() {
                            gl_Position = vec4(in_pos, 0.0, 1.0);
                            uv = in_pos * 0.5 + 0.5;
                        }
                    ''',
                    fragment_shader='''
                        #version 330
                        uniform sampler2D tex;
                        uniform float strength;
                        in vec2 uv;
                        out vec4 fragColor;
                        void main() {
                            vec2 c = uv - 0.5;
                            float r2 = dot(c, c);
                            vec2 d = uv + c * strength * r2;
                            fragColor = texture(tex, d);
                        }
                    '''
                )
                import numpy as _np
                _verts = _np.array([-1, -1, 3, -1, -1, 3], dtype='f4')
                _vbo = _ctx.buffer(_verts)
                _vao = _ctx.simple_vertex_array(_prog, _vbo, 'in_pos')
                _tex = _ctx.texture((hw, hh), 3)
                _tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
                _fbo = _ctx.framebuffer(
                    color_attachments=[_ctx.texture((hw, hh), 3)])
                _prog['strength'].value = 0.55
                _prog['tex'].value = 0
                _gl_ok = True
            except Exception:
                _ctx = None

            # ── numpy 后备 ──
            qw, qh = (hw, hh) if _gl_ok else (int(sw * 0.5), int(sh * 0.5))
            _np_maps = None
            if not _gl_ok:
                try:
                    import numpy as _np
                except ImportError:
                    return
                cx_, cy_ = qw / 2.0, qh / 2.0
                _yy, _xx = _np.mgrid[0:qh, 0:qw].astype(_np.float32)
                _nx = (_xx - cx_) / cx_;  _ny = (_yy - cy_) / cy_
                _r2 = _nx * _nx + _ny * _ny; _f = 1.0 + 0.55 * _r2
                _sx = _np.clip(cx_ + _nx * _f * cx_, 0.0, qw - 1.0001)
                _sy = _np.clip(cy_ + _ny * _f * cy_, 0.0, qh - 1.0001)
                _x0 = _sx.astype(_np.int32); _x1 = _x0 + 1
                _y0 = _sy.astype(_np.int32); _y1 = _y0 + 1
                _wfx = (_sx - _x0).astype(_np.float32)[..., _np.newaxis]
                _wfy = (_sy - _y0).astype(_np.float32)[..., _np.newaxis]
                _np_maps = (_x0, _x1, _y0, _y1, _wfx, _wfy)

            # ── 主循环: 持续产出帧 → _latest_frame ──
            _frame_interval = 0.033  # 目标 ~30fps (降低 CPU 占用)

            # ── 预生成暗色 HUD 叠加层 (SAO 科技感) ──
            _hud_overlay = None
            try:
                from PIL import ImageDraw as _IDraw
                _hud = Image.new('RGBA', (sw, sh), (0, 0, 0, 0))
                _hd = _IDraw.Draw(_hud)
                # 暗色面纱
                _hd.rectangle((0, 0, sw, sh), fill=(0, 0, 0, 115))
                # 横向扫描线 (每 3px 一条, 极淡)
                for _sy2 in range(0, sh, 3):
                    _hd.line([(0, _sy2), (sw, _sy2)], fill=(0, 0, 0, 18))
                # SAO 科技水平线 (上、中、下)
                _line_positions = [
                    int(sh * 0.08), int(sh * 0.15),
                    int(sh * 0.85), int(sh * 0.92),
                ]
                for _ly in _line_positions:
                    _hd.line([(int(sw * 0.05), _ly), (int(sw * 0.95), _ly)],
                             fill=(156, 236, 255, 35), width=1)
                # 中央十字准星 (淡)
                _cx2, _cy2 = sw // 2, sh // 2
                _hd.line([(_cx2 - 40, _cy2), (_cx2 - 12, _cy2)], fill=(156, 236, 255, 45), width=1)
                _hd.line([(_cx2 + 12, _cy2), (_cx2 + 40, _cy2)], fill=(156, 236, 255, 45), width=1)
                _hd.line([(_cx2, _cy2 - 40), (_cx2, _cy2 - 12)], fill=(156, 236, 255, 45), width=1)
                _hd.line([(_cx2, _cy2 + 12), (_cx2, _cy2 + 40)], fill=(156, 236, 255, 45), width=1)
                # 四角 SAO 括号
                _blen = 50
                _bpad = int(sw * 0.04)
                _bpad_y = int(sh * 0.05)
                _bc = (156, 236, 255, 55)
                # 左上
                _hd.line([(_bpad, _bpad_y), (_bpad + _blen, _bpad_y)], fill=_bc, width=1)
                _hd.line([(_bpad, _bpad_y), (_bpad, _bpad_y + _blen)], fill=_bc, width=1)
                # 右上
                _hd.line([(sw - _bpad, _bpad_y), (sw - _bpad - _blen, _bpad_y)], fill=_bc, width=1)
                _hd.line([(sw - _bpad, _bpad_y), (sw - _bpad, _bpad_y + _blen)], fill=_bc, width=1)
                # 左下
                _hd.line([(_bpad, sh - _bpad_y), (_bpad + _blen, sh - _bpad_y)], fill=_bc, width=1)
                _hd.line([(_bpad, sh - _bpad_y), (_bpad, sh - _bpad_y - _blen)], fill=_bc, width=1)
                # 右下
                _hd.line([(sw - _bpad, sh - _bpad_y), (sw - _bpad - _blen, sh - _bpad_y)], fill=_bc, width=1)
                _hd.line([(sw - _bpad, sh - _bpad_y), (sw - _bpad, sh - _bpad_y - _blen)], fill=_bc, width=1)
                _hud_overlay = _hud
            except Exception:
                pass

            while _running[0]:
                _t_start = _time.time()
                shot = _cap_fn()
                if shot is None or not _running[0]:
                    _time.sleep(0.05)
                    continue
                try:
                    if _gl_ok:
                        small = shot.resize((hw, hh), Image.BILINEAR)
                        _tex.write(small.tobytes())
                        _fbo.use()
                        _ctx.clear()
                        _tex.use(0)
                        _vao.render(moderngl.TRIANGLES)
                        raw = _fbo.color_attachments[0].read()
                        dist = Image.frombytes('RGB', (hw, hh), raw)
                    else:
                        tiny = shot.resize((qw, qh), Image.BILINEAR)
                        _x0, _x1, _y0, _y1, _wfx, _wfy = _np_maps
                        a = _np.array(tiny, dtype=_np.float32)
                        t = a[_y0, _x0] * (1 - _wfx) + a[_y0, _x1] * _wfx
                        b = a[_y1, _x0] * (1 - _wfx) + a[_y1, _x1] * _wfx
                        dist = Image.fromarray(
                            (t * (1 - _wfy) + b * _wfy)
                            .clip(0, 255).astype(_np.uint8))
                except Exception:
                    _time.sleep(0.02)
                    continue
                if not _running[0]:
                    break
                full = dist.resize((sw, sh), Image.BILINEAR)
                # 暗色 HUD 叠加 (SAO 科技感背景)
                if _hud_overlay is not None:
                    full = full.convert('RGBA')
                    full = Image.alpha_composite(full, _hud_overlay)
                    full = full.convert('RGB')
                _latest_frame[0] = full
                # 限制帧率, 释放 CPU 给主线程
                _elapsed = _time.time() - _t_start
                _sleep = max(0.001, _frame_interval - _elapsed)
                _time.sleep(_sleep)

            # ── 线程退出, 释放 GPU ──
            if _ctx:
                try: _ctx.release()
                except Exception: pass

        import threading as _th
        _th.Thread(target=_worker, daemon=True).start()
        ov._running_ref = _running

    def _stop_fisheye_overlay(self):
        """销毁持久鱼眼叠加层 (GPU 由后台线程自行释放)."""
        ov = self._fisheye_ov
        self._fisheye_ov = None
        if ov is not None:
            running = getattr(ov, '_running_ref', None)
            if running:
                running[0] = False
            try:
                ov.destroy()
            except Exception:
                pass

    # ══════════════════════════════════════════════
    #  LinkStart 入场鱼眼镜头畅变
    # ══════════════════════════════════════════════
    def _run_fisheye_entry(self):
        """
        LinkStart 结束后短暂鱼眼镜头畟变过渡 — 屏幕从弯曲收缩至正常.

        流程: 抓取当前屏幕 → 应用桶形型畟变 (MESH变换) →
                全屏覆盖层显示畟变图 → 0.9s内渐隐 →
                真实 UI 从底层透出 (SAO 镜头对焦效果).
        """
        try:
            from PIL import ImageGrab, Image, ImageTk
        except ImportError:
            return

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        img = None
        for _grab in (
            lambda: ImageGrab.grab(bbox=(0, 0, sw, sh), all_screens=True),
            lambda: ImageGrab.grab(bbox=(0, 0, sw, sh)),
            lambda: ImageGrab.grab(),
        ):
            try:
                img = _grab()
                break
            except Exception:
                continue
        if img is None:
            return

        # 半分辨率处理 (MESH 运算量 1/4)
        half_w, half_h = sw // 2, sh // 2
        small = img.resize((half_w, half_h), Image.BILINEAR)

        def _barrel(src, strength):
            cx_, cy_ = half_w / 2.0, half_h / 2.0
            grid = 18
            mesh_data = []
            for gy in range(0, half_h, grid):
                for gx in range(0, half_w, grid):
                    x1, y1 = gx, gy
                    x2, y2 = min(gx + grid, half_w), min(gy + grid, half_h)
                    src_pts = []
                    for px, py in [(x1, y1), (x1, y2), (x2, y2), (x2, y1)]:
                        nx_ = (px - cx_) / cx_
                        ny_ = (py - cy_) / cy_
                        r2 = nx_ * nx_ + ny_ * ny_
                        f = 1.0 + strength * r2
                        sx = cx_ + nx_ * f * cx_
                        sy = cy_ + ny_ * f * cy_
                        src_pts.extend([
                            max(0.0, min(half_w - 1.0, sx)),
                            max(0.0, min(half_h - 1.0, sy)),
                        ])
                    mesh_data.append(((x1, y1, x2, y2), src_pts))
            return src.transform(src.size, Image.MESH, mesh_data, Image.BILINEAR)

        try:
            dist_half = _barrel(small, 0.50)
            distorted = dist_half.resize((sw, sh), Image.BILINEAR)
        except Exception:
            return

        # 全屏 overlay
        ov = tk.Toplevel(self.root)
        ov.overrideredirect(True)
        ov.attributes('-topmost', True)
        ov.attributes('-alpha', 1.0)
        ov.geometry(f'{sw}x{sh}+0+0')
        cv_ov = tk.Canvas(ov, width=sw, height=sh,
                          highlightthickness=0, bg='black')
        cv_ov.pack(fill=tk.BOTH, expand=True)
        photo = ImageTk.PhotoImage(distorted)
        cv_ov.create_image(0, 0, image=photo, anchor='nw')
        cv_ov._photo = photo  # 防止 GC

        # ease-in 渐隐: 开始快, 收尾慢 (0.9s 内全透明)
        t0 = time.time()
        dur = 0.90

        def _fade():
            if self._destroyed:
                try: ov.destroy()
                except: pass
                return
            elapsed = time.time() - t0
            if elapsed >= dur:
                try:
                    ov.destroy()
                except Exception:
                    pass
                return
            a = max(0.0, 1.0 - (elapsed / dur) ** 0.6)
            try:
                ov.attributes('-alpha', a)
            except Exception:
                pass
            try:
                self.root.after(16, _fade)
            except Exception:
                pass

        _fade()

    def _play_link_start(self):
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        # 目标位置: 上次保存的位置, 否则右下角
        saved_x = self.settings.get('float_x', None)
        saved_y = self.settings.get('float_y', None)
        # 固定位置: 左下角覆盖整个底部区域 (统一 HUD)
        # 向右偏移 4% 屏宽以覆盖游戏原生 HP/STA 条
        _offset_pct = 0.04
        try:
            if hasattr(self, 'settings') and self.settings:
                _offset_pct = self.settings.get('hud_offset_x', 0.04)
        except Exception:
            pass
        _hp_x = int(sw * _offset_pct)
        _hp_y = sh - self._fh
        if saved_x is not None and saved_y is not None:
            fx_final = max(0, min(int(saved_x), sw - self._fw))
            fy_final = max(0, min(int(saved_y), sh - self._fh))
        else:
            fx_final = _hp_x
            fy_final = _hp_y
        # 起始位置: 屏幕正中央 (LinkStart 动画中心)
        fx_start = sw // 2 - self._fw // 2
        fy_start = sh // 2 + 80   # 略低于中心 (文字下方)

        def on_done():
            self._float.geometry(f'{self._fw}x{self._fh}+{fx_start}+{fy_start}')
            self._set_float_alpha(0.0)
            self._float.deiconify()
            self._float.lift()
            # Re-assert WS_EX_LAYERED (withdraw/deiconify 可能重置)
            try:
                GWL_EXSTYLE = -20
                WS_EX_LAYERED = 0x00080000
                h = self._float_hwnd
                if h:
                    ex = _user32.GetWindowLongW(ctypes.c_void_p(h), GWL_EXSTYLE)
                    if not (ex & WS_EX_LAYERED):
                        _user32.SetWindowLongW(ctypes.c_void_p(h), GWL_EXSTYLE,
                                               ex | WS_EX_LAYERED)
                        print('[SAO-HP] Re-asserted WS_EX_LAYERED after deiconify')
            except Exception:
                pass
            # 立即绘制全透明 ULW 帧, 防止 deiconify 后暴露 Tk 黑底
            try:
                _blank = Image.new('RGBA', (self._fw, self._fh), (0, 0, 0, 0))
                _update_layered_win(self._float_hwnd, _blank, 0)
            except Exception:
                pass
            self._play_motion_blur(closing=False)
            self._run_entry_animation(fx_start, fy_start, fx_final, fy_final)

        # Canvas 渲染 (SAO-UI 隧道模型)
        ls = SAOLinkStart(self.root, on_done=on_done)
        ls.play()

    def _show_welcome_then_menu(self):
        """首次启动: 显示欢迎对话框, 完成后再打开菜单"""
        def on_profile_done(username, profession):
            self._username = username
            self._profession = profession
            self._update_float_title()
            # 更新 SAO 菜单的用户信息
            if self._sao_menu:
                self._sao_menu.username = username
                self._sao_menu.description = profession or 'SAO Auto — 游戏辅助 UI'
            self.root.after(300, self._toggle_sao_menu)

        show_welcome_dialog(self._float, on_done=on_profile_done)

    def _update_float_title(self):
        """更新 HP 组件的用户名"""
        try:
            name = self._username if self._username else 'Player'
            if len(name) > 8:
                name = name[:7] + '…'
            self._hp_display_name = name
            self._refresh_hp_layered()
        except Exception:
            pass

    # ══════════════════════════════════════════════
    #  快捷键
    # ══════════════════════════════════════════════
    def _setup_hotkeys(self):
        self._hotkey_mgr = SAOHotkeyManager(self.settings, {
            'toggle_recognition': lambda: self.root.after(0, self._toggle_recognition_menu),
            'toggle_topmost': lambda: self.root.after(0, self._toggle_topmost),
            'toggle_auto_script': lambda: self.root.after(0, self._toggle_auto_script),
            'hide_panels': lambda: self.root.after(0, self._toggle_hide_all_panels),
            'boss_raid_start': lambda: self.root.after(0, self._toggle_boss_raid),
            'boss_raid_next_phase': lambda: self.root.after(0, self._boss_raid_next_phase),
        })

    def _toggle_recognition_menu(self):
        """切换识别开关 — SAO Entity UI."""
        with self._recog_lock:
            if not self._recognition_active:
                if not self._recognition_engine and not self._recognition_engines:
                    self._start_recognition()
                else:
                    self._recognition_active = True
            else:
                self._recognition_active = False
        self._refresh_menu_if_open()

    def _toggle_auto_script(self, force_enabled=None):
        """切换 AutoKey 脚本开关."""
        config = self._load_auto_key_config()
        if force_enabled is not None:
            config['enabled'] = bool(force_enabled)
        else:
            config['enabled'] = not bool(config.get('enabled', False))
        self._save_auto_key_config(config)
        state_text = 'ON' if config['enabled'] else 'OFF'
        print(f'[SAO Entity] AUTO KEY: {state_text}')
        self._refresh_menu_if_open()

    def _toggle_boss_raid(self, force_enabled=None):
        """切换 Boss Raid 引擎开关."""
        if not self._boss_raid_engine:
            return
        config = self._load_boss_raid_config()
        if force_enabled is not None:
            config['enabled'] = bool(force_enabled)
        else:
            config['enabled'] = not bool(config.get('enabled', False))
        self._save_boss_raid_config(config)
        state_text = 'ON' if config['enabled'] else 'OFF'
        print(f'[SAO Entity] BOSS RAID: {state_text}')
        self._refresh_menu_if_open()

    def _boss_raid_next_phase(self):
        """Boss Raid 下一阶段."""
        if not self._boss_raid_engine:
            return
        self._boss_raid_engine.next_phase()

    # ── AutoKey config helpers ──
    def _auto_key_settings_ref(self):
        return self._cfg_settings_ref or self.settings

    def _auto_key_author_snapshot(self):
        gs = getattr(self, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {'player_uid': '', 'player_name': self._username,
                'profession_id': 0, 'profession_name': self._profession}

    def _load_auto_key_config(self):
        ref = self._auto_key_settings_ref()
        return load_auto_key_config(ref, state_snapshot=self._auto_key_author_snapshot())

    def _save_auto_key_config(self, config):
        ref = self._auto_key_settings_ref()
        saved = save_auto_key_config(ref, config)
        if self._auto_key_engine:
            self._auto_key_engine.invalidate()
        return saved

    # ── BossRaid config helpers ──
    def _boss_raid_settings_ref(self):
        return self._cfg_settings_ref or self.settings

    def _boss_raid_author_snapshot(self):
        gs = getattr(self, '_game_state', None)
        if gs is not None:
            return snapshot_author_from_state(gs)
        return {'player_uid': '', 'player_name': self._username,
                'profession_id': 0, 'profession_name': self._profession}

    def _load_boss_raid_config(self):
        ref = self._boss_raid_settings_ref()
        return load_boss_raid_config(ref)

    def _save_boss_raid_config(self, config):
        ref = self._boss_raid_settings_ref()
        return save_boss_raid_config(ref, config)

    # ── Settings helper ──
    def _set_setting(self, key: str, value):
        """Persist a setting to cfg_settings and save."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            self._cfg_settings_ref.set(key, value)
            try: self._cfg_settings_ref.save()
            except Exception: pass

    def _get_setting(self, key: str, default=None):
        """Read a setting."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

    # ── Sound / Display 开关 ──

    def _toggle_sound_enabled(self):
        from sao_sound import set_sound_enabled, get_sound_enabled
        new = not get_sound_enabled()
        set_sound_enabled(new)
        self._set_setting('sound_enabled', new)

    def _adj_sound_volume(self, delta: int):
        from sao_sound import set_sound_volume, get_sound_volume
        cur = get_sound_volume()
        nv = max(0, min(100, cur + delta))
        set_sound_volume(nv)
        self._set_setting('sound_volume', nv)

    def _toggle_dps_enabled(self):
        cur = bool(self._get_setting('dps_enabled', True))
        new = not cur
        self._set_setting('dps_enabled', new)
        self._dps_visible = new
        if self._dps_overlay:
            if new:
                self._dps_overlay.show()
            else:
                self._dps_overlay.hide()

    def _toggle_burst_enabled(self):
        cur = bool(self._get_setting('burst_enabled', True))
        self._set_setting('burst_enabled', not cur)

    def _cycle_boss_bar_mode(self):
        modes = ['boss_raid', 'always', 'off']
        cur = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        idx = modes.index(cur) if cur in modes else 0
        nxt = modes[(idx + 1) % len(modes)]
        self._set_setting('boss_bar_mode', nxt)

    # ══════════════════════════════════════════════
    #  其他功能
    # ══════════════════════════════════════════════
    def _toggle_topmost(self):
        current = self._float.attributes('-topmost')
        new_val = not current
        self._float.attributes('-topmost', new_val)
        for panel in [self._piano_panel, self._viz_panel, self._status_panel]:
            try:
                if panel and panel.winfo_exists():
                    panel.attributes('-topmost', new_val)
            except Exception:
                pass
        self._refresh_menu_if_open()

    def _switch_to_webview_ui(self):
        """切换到 WebView UI (sao_webview.py) — 热切换"""
        def _do_switch():
            self.settings.set('ui_mode', 'webview')
            self.settings.save()

            def _launch_next():
                import gc; gc.collect()
                time.sleep(0.3)
                try:
                    from sao_webview import SAOWebViewGUI
                    app = SAOWebViewGUI()
                    app.run()
                except Exception as e:
                    print(f"[SAO] Hot switch to WebView failed: {e}")
                    import traceback; traceback.print_exc()

            self._after_shutdown = _launch_next
            self._run_exit_animation(after_shutdown=None,
                                     mode='switch', target_label='SAO WEBVIEW UI')

        SAODialog.ask(self._float, "切换 UI",
                      "将切换到 SAO WebView UI。\n确定继续吗？",
                      on_ok=_do_switch)

    def _switch_to_old_ui(self):
        """Old UI 已移除 — no-op"""
        pass

    def _show_about(self):
        if self._sao_menu.visible:
            self._sao_menu.close()
        self.root.after(600, lambda: SAODialog.showinfo(
            self._float, "关于",
            f"SAO Auto — 游戏辅助 UI\n{APP_VERSION_LABEL}\n\n"
            "Alt+A 打开 SAO 菜单\n"
            "右键悬浮按钮查看更多选项"))

    def _edit_profile(self):
        """打开角色资料编辑对话框"""
        if self._sao_menu.visible:
            self._sao_menu.close()

        def on_profile_done(username, profession):
            self._username = username
            self._profession = profession
            self._update_float_title()
            if self._sao_menu:
                self._sao_menu.username = username
                self._sao_menu.description = profession or 'SAO Auto — 游戏辅助 UI'
            if self._player_panel:
                self._player_panel._username = username
                self._player_panel._profession = profession
                if self._player_panel._active:
                    self._player_panel._redraw_top(
                        self._player_panel._target_w,
                        self._player_panel._top_h)

        self.root.after(600, lambda: show_welcome_dialog(
            self._float, on_done=on_profile_done))

    def _show_leaderboard(self):
        """排行榜已移除 — no-op"""
        pass

    def _cleanup_exit_overlay(self):
        ov = getattr(self, '_exit_overlay', None)
        if not ov:
            return
        try:
            gl = ov.get('gl')
            if gl:
                for key in ('pulse_tex', 'pulse_fbo', 'pulse_prog', 'pulse_vao', 'ctx'):
                    try:
                        obj = gl.get(key)
                        if obj is not None:
                            obj.release()
                    except Exception:
                        pass
        except Exception:
            pass
        try:
            win = ov.get('win')
            if win and win.winfo_exists():
                win.destroy()
        except Exception:
            pass
        self._exit_overlay = None

    def _cleanup_entry_overlay(self):
        ov = getattr(self, '_entry_overlay', None)
        if not ov:
            return
        try:
            gl = ov.get('gl') or {}
            for key in ('boot_fbo', 'boot_tex', 'boot_vao', 'boot_prog', 'ctx'):
                obj = gl.get(key)
                if obj is not None:
                    try:
                        obj.release()
                    except Exception:
                        pass
        except Exception:
            pass
        try:
            win = ov.get('win')
            if win and win.winfo_exists():
                win.destroy()
        except Exception:
            pass
        self._entry_overlay = None

    def _init_entry_boot_gl(self, width, height):
        try:
            import moderngl
        except Exception:
            return None
        try:
            ctx = moderngl.create_standalone_context()
            prog = ctx.program(
                vertex_shader='''
#version 330
out vec2 uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
in vec2 uv;
uniform vec2 u_resolution;
uniform float u_progress;
out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float band(float x, float center, float width) {
    return exp(-pow((x - center) / max(0.0001, width), 2.0));
}

void main() {
    vec2 p = uv - 0.5;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(p);
    float progress = clamp(u_progress, 0.0, 1.0);

    float openA = smoothstep(0.00, 0.22, progress);
    float openB = smoothstep(0.12, 0.56, progress);
    float settle = smoothstep(0.40, 1.00, progress);
    // barrel distortion strongest at ignition, flattens as screen opens
    float barrelK = mix(0.44, 0.0, smoothstep(0.0, 0.34, progress));
    vec2 bv = uv - 0.5;
    vec2 distUv = uv + bv * barrelK * dot(bv, bv);

    float halfH = mix(0.003, 0.50, openA);
    float halfW = mix(0.030, 0.62, openB);
    float maskY = 1.0 - smoothstep(halfH, halfH + 0.030, abs(distUv.y - 0.5));
    float maskX = 1.0 - smoothstep(halfW, halfW + 0.045, abs(distUv.x - 0.5));
    float screenMask = clamp(maskX * maskY, 0.0, 1.0);

    // overexposure flash: floods full frame at the moment the screen fires on
    float overexpose = smoothstep(0.0, 0.06, progress) * (1.0 - smoothstep(0.14, 0.40, progress));

    float ignition = band(uv.y, 0.5, mix(0.0016, 0.020, openA)) * (1.0 - smoothstep(0.18, 0.46, progress));
    float flare = exp(-r * mix(24.0, 6.5, openB)) * (0.45 + 0.55 * (1.0 - settle));
    float scan = 0.92 + 0.08 * sin((uv.y * u_resolution.y) * 2.8 + progress * 1100.0);
    float noise = hash21(gl_FragCoord.xy * 0.03 + progress * 17.0) * 0.05;
    float bezel = smoothstep(0.90, 0.18, max(abs(p.x) * 0.92, abs(p.y) * 1.25));
    float sweep = band(uv.y, 0.26 + progress * 0.48, 0.045) + band(uv.y, 0.60 + progress * 0.12, 0.060) * 0.55;

    vec3 cyan = vec3(0.52, 0.92, 1.0);
    vec3 blue = vec3(0.08, 0.46, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 color = vec3(0.0);
    // full-frame overexposure bloom + cyan tint bleed at ignition
    color += white * overexpose * 2.60;
    color += vec3(0.70, 0.94, 1.0) * overexpose * exp(-r * 3.0) * 1.40;
    color += white * ignition * 1.6;
    color += mix(blue, cyan, 0.50) * flare * (0.65 + 0.35 * openB);
    color += cyan * screenMask * (0.18 + 0.24 * sweep + 0.18 * settle);
    color += white * screenMask * 0.10 * (1.0 - smoothstep(0.0, 0.4, r));
    color += vec3(0.78, 0.96, 1.0) * sweep * screenMask * 0.24;
    color *= scan * bezel;
    color += vec3(noise) * screenMask * 0.12;
    color = clamp(color, 0.0, 1.0);
    fragColor = vec4(color, 1.0);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 3)
            fbo = ctx.framebuffer(color_attachments=[tex])
            prog['u_resolution'].value = (float(width), float(height))
            return {
                'ctx': ctx,
                'boot_prog': prog,
                'boot_vao': vao,
                'boot_tex': tex,
                'boot_fbo': fbo,
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            return None

    def _draw_entry_boot_gl(self, cv, ov, progress):
        gl = ov.get('gl')
        if not gl:
            return False
        try:
            prog = gl['boot_prog']
            fbo = gl['boot_fbo']
            vao = gl['boot_vao']
            fbo.use()
            gl['ctx'].clear(0.0, 0.0, 0.0, 1.0)
            prog['u_progress'].value = float(max(0.0, min(1.0, progress)))
            vao.render()
            raw = fbo.read(components=3)
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(ov['sh'], ov['sw'], 3)
            photo = ImageTk.PhotoImage(Image.fromarray(arr[::-1], 'RGB'))
            ov['gl_photo'] = photo
            cv.create_image(0, 0, image=photo, anchor='nw')
            return True
        except Exception:
            return False

    def _create_entry_overlay(self, start_x, start_y, end_x, end_y):
        self._cleanup_entry_overlay()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        ov = tk.Toplevel(self.root)
        ov.overrideredirect(True)
        ov.attributes('-topmost', True)
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#060a10')
        ov.attributes('-alpha', 0.0)
        try:
            _disable_native_window_shadow(ov)
        except Exception:
            pass
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#060a10', highlightthickness=0, bd=0)
        cv.pack(fill=tk.BOTH, expand=True)
        self._entry_overlay = {
            'win': ov,
            'cv': cv,
            'sw': sw,
            'sh': sh,
            'gl': self._init_entry_boot_gl(sw, sh),
            'gl_photo': None,
            'start_x': start_x + self._fw // 2,
            'start_y': start_y + self._fh // 2,
            'end_x': end_x + self._fw // 2,
            'end_y': end_y + self._fh // 2,
        }
        return self._entry_overlay

    def _draw_entry_overlay(self, progress):
        ov = getattr(self, '_entry_overlay', None)
        if not ov:
            return
        try:
            win = ov['win']
            cv = ov['cv']
            if not win.winfo_exists() or not cv.winfo_exists():
                return
        except Exception:
            return

        sw, sh = ov['sw'], ov['sh']
        stage1 = min(1.0, progress / 0.34)
        stage2 = max(0.0, min(1.0, (progress - 0.24) / 0.76))
        bloom = ease_out(stage1)
        deploy = ease_in_out(stage2)
        cx = int(lerp(ov['start_x'], ov['end_x'], deploy))
        cy = int(lerp(ov['start_y'], ov['end_y'], deploy))
        cyan = '#86dfff'
        gold = '#f3af12'
        white = '#edf7ff'
        dim_cyan = '#173746'
        dim_gold = '#5e4211'
        # TV-on: 0→1 during 0–0.62, settled: 1.0 during 0.62–0.80, TV-close: 1→0 during 0.80–1.0
        if progress <= 0.62:
            boot_t = progress / 0.62
        elif progress <= 0.80:
            boot_t = 1.0
        else:
            boot_t = max(0.0, 1.0 - (progress - 0.80) / 0.20)
        tv_close_f = max(0.0, (progress - 0.80) / 0.20)  # 0→1 during close phase

        try:
            base_alpha = max(0.0, min(0.95, (1.0 - progress) ** 0.28 * 0.92))
            # fade window to near-zero as TV screen collapses
            win.attributes('-alpha', max(0.0, base_alpha * (1.0 - tv_close_f * 0.95)))
        except Exception:
            pass

        cv.delete('all')
        boot_gl_drawn = self._draw_entry_boot_gl(cv, ov, boot_t)
        if not boot_gl_drawn:
            scan_pitch = 24
            scan_shift = int((progress * 240) % scan_pitch)
            for y in range(-scan_pitch, sh + scan_pitch, scan_pitch):
                yy = y + scan_shift
                col = dim_cyan if ((y // scan_pitch) % 2 == 0) else '#101823'
                cv.create_line(0, yy, sw, yy, fill=col, width=1)

        # suppress all Canvas HUD elements once TV-close is under way
        if tv_close_f > 0.08:
            return

        span = int(lerp(min(sw * 0.42, 520), min(sw * 0.22, 260), deploy))
        aperture = int(lerp(172, 28, deploy))
        for off, col in [(-54, cyan), (-24, dim_cyan), (24, dim_gold), (54, gold)]:
            cv.create_line(cx - span, cy + off, cx - aperture, cy + off, fill=col, width=1)
            cv.create_line(cx + aperture, cy + off, cx + span, cy + off, fill=col, width=1)

        ring_r = int(lerp(220, 64, deploy))
        for extra, col in [(0, cyan), (24, gold)]:
            r = ring_r + extra
            arm = 22 + extra // 4
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = cx + sx * r
                    py = cy + sy * r
                    cv.create_line(px, py, px - sx * arm, py, fill=col, width=1)
                    cv.create_line(px, py, px, py - sy * arm, fill=col, width=1)

        diamond = int(lerp(28, 10, deploy))
        cv.create_polygon(cx, cy - diamond, cx + diamond, cy,
                          cx, cy + diamond, cx - diamond, cy,
                          outline=white, fill='')
        cv.create_line(cx - 46, cy, cx + 46, cy, fill=white, width=1)
        cv.create_line(cx, cy - 22, cx, cy + 22, fill=white, width=1)

        if not boot_gl_drawn or progress > 0.28:
            pulse_y = int(lerp(cy - 160, cy + 88, bloom))
            cv.create_line(max(0, cx - span - 150), pulse_y,
                           min(sw, cx + span + 150), pulse_y,
                           fill=cyan, width=1)
            cv.create_line(max(0, cx - span - 110), pulse_y + 3,
                           min(sw, cx + span + 110), pulse_y + 3,
                           fill=dim_cyan, width=1)

        label_x1 = max(30, cx - span - 70)
        label_x2 = min(sw - 30, cx + span + 70)
        cv.create_text(label_x1, max(24, cy - 164), text='SYS:ENTITY',
                       anchor='w', fill=cyan, font=('Consolas', 9))
        cv.create_text(label_x2, max(24, cy - 164), text='SEQ:ENTRY',
                       anchor='e', fill=gold, font=('Consolas', 9))
        cv.create_text(label_x1, min(sh - 24, cy + 174), text='STATUS:DEPLOY',
                       anchor='w', fill=dim_cyan, font=('Consolas', 9))
        cv.create_text(label_x2, min(sh - 24, cy + 174), text=time.strftime('%H:%M:%S'),
                       anchor='e', fill=dim_gold, font=('Consolas', 9))

        text_y = cy + 92
        cv.create_text(cx, text_y, text='LINK START', fill=white,
                       font=get_sao_font(16, True))
        cv.create_text(cx, text_y + 26, text='ENTITY DEPLOYMENT', fill=gold,
                       font=('Consolas', 11, 'bold'))
        cv.create_text(cx, text_y + 48, text='INITIALIZING VISUAL SHELL',
                       fill='#8aaec0', font=('Consolas', 9))

    def _run_entry_animation(self, fx_start, fy_start, fx_final, fy_final):
        self._create_entry_overlay(fx_start, fy_start, fx_final, fy_final)
        anim_start = time.time()
        total = 1.28
        phase1 = 0.44

        def _done():
            self._cleanup_entry_overlay()
            self._breath_base_x = fx_final
            self._breath_base_y = fy_final
            # self.root.after(120, self._start_float_breath)  # 禁用浮动
            self.root.after(160, self._animate_float_hud)
            # 启动识别循环
            self.root.after(200, self._start_recognition)
            self.root.after(600, self._recognition_loop)
            if not self._username:
                self.root.after(420, self._show_welcome_then_menu)
            else:
                self.root.after(420, self._toggle_sao_menu)
            self.root.after(900, self._restore_panels)

        def _tick():
            if self._destroyed:
                self._cleanup_entry_overlay()
                return
            try:
                if not self._float.winfo_exists():
                    self._cleanup_entry_overlay()
                    return
            except Exception:
                self._cleanup_entry_overlay()
                return

            elapsed = time.time() - anim_start
            t = min(1.0, elapsed / total)
            self._draw_entry_overlay(t)

            if elapsed < phase1:
                hold = ease_out(elapsed / phase1)
                self._set_float_alpha(0.15 * hold)
                try:
                    self.root.after(16, _tick)
                except Exception:
                    self._cleanup_entry_overlay()
                return

            deploy = min(1.0, (elapsed - phase1) / max(0.001, total - phase1))
            deploy_e = ease_out(deploy)
            fx = int(lerp(fx_start, fx_final, deploy_e))
            fy = int(lerp(fy_start, fy_final, deploy_e))
            self._float.geometry(f'+{fx}+{fy}')
            self._set_float_alpha(0.95 * ease_in_out(deploy))

            if elapsed < total:
                try:
                    self.root.after(16, _tick)
                except Exception:
                    self._cleanup_entry_overlay()
            else:
                self._float.geometry(f'+{fx_final}+{fy_final}')
                self._set_float_alpha(0.95)
                _done()

        _tick()

    def _get_exit_banner(self, mode='exit', target_label=None):
        if mode == 'switch':
            return {
                'primary': 'INTERFACE SHIFT',
                'secondary': (target_label or 'NEXT UI').upper(),
                'tertiary': 'TRANSFERRING CONTROL TO NEXT LAYER',
                'accent': '#f3af12',
                'accent_dim': '#5e4211',
            }
        return {
            'primary': 'SYSTEM LOG OUT',
            'secondary': 'SAO ENTITY',
            'tertiary': 'PERSISTING SESSION STATE',
            'accent': '#86dfff',
            'accent_dim': '#173746',
        }

    def _init_exit_pulse_gl(self, width, height):
        try:
            import moderngl
        except Exception:
            return None
        try:
            ctx = moderngl.create_standalone_context()
            prog = ctx.program(
                vertex_shader='''
#version 330
out vec2 uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
in vec2 uv;
uniform vec2 u_resolution;
uniform vec2 u_center;
uniform float u_progress;
out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float band(float x, float center, float width) {
    return exp(-pow((x - center) / max(0.0001, width), 2.0));
}

float gridLine(vec2 q, vec2 dir, float scale, float width) {
    float v = abs(fract(dot(q, dir) * scale) - 0.5);
    return 1.0 - smoothstep(width, width + 0.018, v);
}

void main() {
    vec2 center = u_center / u_resolution;
    vec2 p = uv - center;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(p);
    float progress = clamp(u_progress, 0.0, 1.0);

    float bluePulse = smoothstep(0.00, 0.09, progress) * (1.0 - smoothstep(0.12, 0.30, progress));
    float whitePulse = smoothstep(0.14, 0.24, progress) * (1.0 - smoothstep(0.28, 0.46, progress));
    float exposure = smoothstep(0.00, 0.08, progress) * (1.0 - smoothstep(0.12, 0.32, progress));
    float edgeFlood = smoothstep(0.18, 0.74, progress);
    float tvClose = smoothstep(0.80, 1.0, progress);

    float fracture1 = band(uv.y, 0.26 + progress * 0.28, 0.014) * step(0.34, hash21(vec2(floor(uv.x * 180.0), floor(progress * 80.0) + 7.0)));
    float fracture2 = band(uv.y, 0.60 - progress * 0.18, 0.018) * step(0.42, hash21(vec2(floor(uv.x * 140.0) + 9.0, floor(progress * 110.0) + 3.0)));
    float fracture = fracture1 + fracture2;
    vec2 radialDir = normalize(p + vec2(0.0001, 0.0));
    vec2 hudBreak = vec2(fracture * (0.020 + whitePulse * 0.018), fracture * 0.0015);

    float ringRadius = mix(0.012, 0.74, progress);
    float ringWidth = mix(0.040, 0.014, progress);
    float refractBand = band(r, ringRadius - mix(0.015, 0.060, progress), ringWidth * 3.6)
                      * smoothstep(0.05, 0.78, progress);
    vec2 distort = radialDir * refractBand * (0.022 + whitePulse * 0.020)
                 + hudBreak
                 + vec2(sin(uv.y * u_resolution.y * 0.090 + progress * 28.0),
                        cos(uv.x * u_resolution.x * 0.052 - progress * 21.0)) * refractBand * 0.0045;

    // layered HUD slab displacement: horizontal bands shift left/right in alternating direction
    // each activates after the pulse ring sweeps through its Y position
    float s1 = band(uv.y, 0.24, 0.022) * smoothstep(0.10, 0.22, progress);
    float s2 = band(uv.y, 0.43, 0.018) * smoothstep(0.16, 0.28, progress);
    float s3 = band(uv.y, 0.66, 0.024) * smoothstep(0.20, 0.34, progress);
    float s4 = band(uv.y, 0.81, 0.016) * smoothstep(0.26, 0.40, progress);
    float slabFade = 1.0 - smoothstep(0.78, 0.96, progress);
    distort.x += (s1 * 0.032 - s2 * 0.024 + s3 * 0.028 - s4 * 0.019) * slabFade;
    distort.y += (s1 * 0.005 - s3 * 0.004) * slabFade;
    float slabEdge = max(max(s1, s2), max(s3, s4)) * slabFade;

    vec2 rp = p + distort;
    float rr = length(rp);
    float rang = atan(rp.y, rp.x);

    float fringe = (0.012 + bluePulse * 0.012 + whitePulse * 0.018) * (0.35 + rr * 2.1);
    float segmentA = smoothstep(0.12, 0.94, 0.5 + 0.5 * sin(rang * 16.0 + progress * 18.0 + rr * 34.0));
    float segmentB = smoothstep(0.20, 0.97, 0.5 + 0.5 * cos(rang * 10.0 - progress * 16.0 - rr * 24.0));
    float segmentMask = clamp(segmentA * 0.65 + segmentB * 0.95, 0.0, 1.0);

    float ringR = band(rr, ringRadius + fringe * 1.06, ringWidth * 1.08) * (0.45 + 0.55 * segmentMask);
    float ringG = band(rr, ringRadius, ringWidth) * (0.26 + 0.74 * segmentMask);
    float ringB = band(rr, max(0.0, ringRadius - fringe), ringWidth * 0.88) * (0.36 + 0.64 * segmentMask);

    float echoR = band(rr, ringRadius + 0.030, ringWidth * 1.6) * (1.0 - smoothstep(0.24, 0.82, progress));
    float echoC = band(rr, max(0.0, ringRadius - 0.080), ringWidth * 2.8) * (1.0 - smoothstep(0.14, 0.62, progress));
    float echoB = band(rr, max(0.0, ringRadius - 0.136), ringWidth * 3.7) * (1.0 - smoothstep(0.10, 0.52, progress));

    vec2 dirA = normalize(vec2(1.0, 0.0));
    vec2 dirB = normalize(vec2(0.5, 0.8660254));
    vec2 dirC = normalize(vec2(-0.5, 0.8660254));
    float hexScale = mix(21.0, 32.0, smoothstep(0.08, 0.82, progress));
    float lineA = gridLine(rp, dirA, hexScale, 0.035);
    float lineB = gridLine(rp, dirB, hexScale, 0.033);
    float lineC = gridLine(rp, dirC, hexScale, 0.033);
    float hexWire = max(lineA, max(lineB, lineC));
    float hexNode = max(lineA * lineB, max(lineB * lineC, lineC * lineA));
    float hexMask = band(rr, ringRadius + 0.018, ringWidth * 5.0)
                  + band(rr, ringRadius - 0.096, ringWidth * 6.6) * 0.8;
    float chainWave = band(fract((rang / 6.2831853) + progress * 1.55), 0.5, 0.18);
    float nodeCascade = hexNode * hexMask * (0.35 + 0.65 * chainWave);
    float circuitry = clamp((hexWire * 0.58 + nodeCascade * 1.22), 0.0, 1.0);

    float core = exp(-rr * mix(62.0, 24.0, progress)) * (0.76 + 0.24 * whitePulse);
    float bloom = exp(-rr * 7.0) * (bluePulse * 0.90 + whitePulse * 1.10 + exposure * 0.58);
    float halo = exp(-rr * 2.8) * smoothstep(0.04, 0.22, progress) * (1.0 - smoothstep(0.58, 1.0, progress));

    vec2 edgeUv = abs(uv - 0.5) * 2.0;
    float edgeWave = band(rr, mix(0.18, 1.04, progress), mix(0.10, 0.020, progress));
    float edgeMask = pow(max(edgeUv.x, edgeUv.y), 3.0);
    float edgeSweep = edgeWave * edgeMask * edgeFlood;
    float edgeGlow = pow(max(edgeUv.x, edgeUv.y), 2.1) * (0.10 + 0.90 * edgeFlood) * (1.0 - tvClose * 0.55);

    float scanlines = 0.90 + 0.10 * sin((uv.y * u_resolution.y + progress * 2900.0) * 1.14);
    float scanMicro = 0.95 + 0.05 * sin((uv.y * u_resolution.y) * 4.2 + progress * 970.0);
    float lensSweep = band(uv.y, 0.24 + progress * 0.54, 0.045) + band(uv.y, 0.64 - progress * 0.10, 0.062) * 0.6;

    float tearBand1 = band(uv.y, 0.33 + 0.09 * sin(progress * 12.0), 0.012 + whitePulse * 0.008);
    float tearBand2 = band(uv.y, 0.61 + 0.06 * cos(progress * 10.0 + 0.7), 0.015 + bluePulse * 0.010);
    float tearPattern1 = step(0.38, hash21(vec2(floor((uv.x + distort.x * 9.0) * 220.0), floor(progress * 90.0) + 13.0)));
    float tearPattern2 = step(0.32, hash21(vec2(floor((uv.x - distort.x * 7.0) * 180.0) + 7.0, floor(progress * 126.0) + 27.0)));
    float tear = tearBand1 * tearPattern1 + tearBand2 * tearPattern2;

    float closeH = mix(0.50, 0.010, tvClose);
    float closeW = mix(0.50, 0.022, tvClose);
    float tvMaskY = 1.0 - smoothstep(closeH, closeH + 0.02, abs(uv.y - 0.5));
    float tvMaskX = 1.0 - smoothstep(closeW, closeW + 0.02, abs(uv.x - 0.5));
    float tvMask = mix(1.0, tvMaskY, smoothstep(0.80, 0.92, progress));
    tvMask *= mix(1.0, tvMaskX, smoothstep(0.90, 1.0, progress));

    float grain = hash21(gl_FragCoord.xy * 0.05 + progress * 31.0) * 0.045;
    vec3 cyan = vec3(0.60, 0.95, 1.0);
    vec3 blue = vec3(0.06, 0.48, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 ghost = vec3(0.34, 0.88, 1.0);
    vec3 color = vec3(0.0);

    color += mix(blue, cyan, 0.42) * exposure * (0.54 + 0.46 * exp(-rr * 2.0));
    color += vec3(0.82, 0.96, 1.0) * lensSweep * (0.08 + exposure * 0.22);
    color += white * whitePulse * (0.26 + 0.74 * exp(-rr * 3.2));
    color.r += ringR * 0.96 + echoR * 0.46 + whitePulse * 0.18;
    color.g += ringG * 1.02 + echoC * 0.40 + circuitry * 0.26;
    color.b += ringB * 1.86 + echoC * 0.66 + echoB * 0.58 + edgeSweep * 0.96 + circuitry * 0.40;
    color += cyan * bloom * 1.02;
    color += ghost * (echoC * 0.64 + echoB * 0.46 + halo * 0.60);
    color += vec3(0.40, 0.90, 1.0) * edgeGlow * 0.65;
    color += vec3(0.50, 0.94, 1.0) * edgeSweep;
    color += vec3(0.54, 0.96, 1.0) * circuitry * (0.48 + bluePulse * 0.60);
    color += vec3(0.92, 1.0, 1.0) * nodeCascade * 0.40;
    color += vec3(0.60, 0.96, 1.0) * slabEdge * (0.34 + whitePulse * 0.24) * (1.0 - tvClose);
    color += vec3(0.80, 0.98, 1.0) * tear * (0.42 + exposure * 0.48);
    color += white * core * (0.24 + whitePulse * 0.42);
    color += vec3(grain) * (0.18 + tear * 0.22 + circuitry * 0.08);

    color *= scanlines * scanMicro;
    color += vec3(0.08, 0.18, 0.36) * fracture * 0.32;
    color *= tvMask;
    color = clamp(color, 0.0, 1.0);
    fragColor = vec4(color, 1.0);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 3)
            fbo = ctx.framebuffer(color_attachments=[tex])
            prog['u_resolution'].value = (float(width), float(height))
            return {
                'ctx': ctx,
                'pulse_prog': prog,
                'pulse_vao': vao,
                'pulse_tex': tex,
                'pulse_fbo': fbo,
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            return None

    def _draw_exit_pulse_gl(self, cv, ov, cx, cy, purge_t):
        gl = ov.get('gl')
        if not gl:
            return False
        try:
            prog = gl['pulse_prog']
            fbo = gl['pulse_fbo']
            vao = gl['pulse_vao']
            fbo.use()
            gl['ctx'].clear(0.0, 0.0, 0.0, 1.0)
            prog['u_center'].value = (float(cx), float(cy))
            prog['u_progress'].value = float(max(0.0, min(1.0, purge_t)))
            vao.render()
            raw = fbo.read(components=3)
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(ov['sh'], ov['sw'], 3)
            photo = ImageTk.PhotoImage(Image.fromarray(arr[::-1], 'RGB'))
            ov['gl_photo'] = photo
            cv.create_image(0, 0, image=photo, anchor='nw')
            return True
        except Exception:
            return False

    def _create_exit_overlay(self, mode='exit', target_label=None):
        self._cleanup_exit_overlay()
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
        except Exception:
            sw, sh = 1920, 1080
        try:
            ov = tk.Toplevel(self.root)
            ov.overrideredirect(True)
        except Exception:
            self._finalize_close()
            return None
        ov.attributes('-topmost', True)
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#060a10')
        ov.attributes('-alpha', 0.0)
        try:
            _disable_native_window_shadow(ov)
        except Exception:
            pass
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#060a10', highlightthickness=0, bd=0)
        cv.pack(fill=tk.BOTH, expand=True)
        try:
            fx = self._float.winfo_rootx() + self._fw // 2
            fy = self._float.winfo_rooty() + self._fh // 2
        except Exception:
            fx, fy = sw // 2, sh // 2
        self._exit_overlay = {
            'win': ov,
            'cv': cv,
            'sw': sw,
            'sh': sh,
            'fx': fx,
            'fy': fy,
            'banner': self._get_exit_banner(mode, target_label),
            'mode': mode,
            'gl': self._init_exit_pulse_gl(sw, sh),
            'gl_photo': None,
        }
        return self._exit_overlay

    def _draw_exit_overlay(self, progress):
        ov = getattr(self, '_exit_overlay', None)
        if not ov:
            return
        try:
            win = ov['win']
            cv = ov['cv']
            if not win.winfo_exists() or not cv.winfo_exists():
                return
        except Exception:
            return

        sw, sh = ov['sw'], ov['sh']
        cx, cy = ov['fx'], ov['fy']
        lock_t = min(1.0, progress / 0.34)
        purge_t = max(0.0, min(1.0, (progress - 0.34) / 0.66))
        lock_e = ease_out(lock_t)
        purge_e = ease_in_out(purge_t)
        cyan = '#86dfff'
        gold = ov['banner']['accent']
        dim_cyan = '#173746'
        dim_gold = ov['banner']['accent_dim']
        white = '#edf7ff'

        wash = 0.22 + 0.78 * lock_e
        sweep = ((lock_t * 0.45) + purge_t * 1.2) % 1.0
        tv_fade = 0.0 if progress <= 0.82 else min(1.0, max(0.0, (progress - 0.82) / 0.18))
        tv_fade = ease_in_out(tv_fade)

        try:
            # fade window all the way to transparent as TV-close completes
            peak_alpha = min(0.97, 0.12 + 0.58 * lock_e + 0.20 * purge_e)
            win.attributes('-alpha', max(0.0, peak_alpha * (1.0 - tv_fade * 0.97)))
        except Exception:
            pass

        cv.delete('all')
        if purge_t < 0.02 or not ov.get('gl'):
            scan_pitch = 26
            scan_shift = int((progress * 280) % scan_pitch)
            for y in range(-scan_pitch, sh + scan_pitch, scan_pitch):
                yy = y + scan_shift
                col = dim_cyan if ((y // scan_pitch) % 2 == 0) else '#101823'
                cv.create_line(0, yy, sw, yy, fill=col, width=1)

        pulse_gl_drawn = False
        if purge_t > 0.0:
            pulse_gl_drawn = self._draw_exit_pulse_gl(cv, ov, cx, cy, purge_t)
            if not pulse_gl_drawn:
                pulse = max(0.0, 1.0 - abs(purge_t - 0.18) / 0.18)
                if pulse > 0.01:
                    if pulse > 0.72:
                        flash_fill = '#eefbff'
                        flash_stipple = 'gray25'
                    elif pulse > 0.38:
                        flash_fill = '#c8efff'
                        flash_stipple = 'gray25'
                    else:
                        flash_fill = '#8edfff'
                        flash_stipple = 'gray50'
                    cv.create_rectangle(0, 0, sw, sh, fill=flash_fill, outline='', stipple=flash_stipple)
                    bloom_r = int(lerp(40, min(sw, sh) * 0.32, pulse))
                    core_r = max(10, int(bloom_r * 0.26))
                    cv.create_oval(cx - bloom_r, cy - bloom_r,
                                   cx + bloom_r, cy + bloom_r,
                                   outline='#dff8ff', width=max(1, int(2 + pulse * 3)),
                                   stipple='gray25')
                    cv.create_oval(cx - core_r, cy - core_r,
                                   cx + core_r, cy + core_r,
                                   fill='#f8feff', outline='', stipple='gray25')

                if tv_fade > 0.0:
                    fade = int(255 * tv_fade)
                    fill = f'#{fade:02x}{fade:02x}{fade:02x}'
                    cv.create_rectangle(0, 0, sw, sh, fill=fill, outline='',
                                        stipple='gray50' if tv_fade < 0.7 else '')

        # suppress Canvas HUD once TV-close is visually dominant
        if tv_fade > 0.25:
            return

        span = int(lerp(min(sw * 0.30, 360), min(sw * 0.38, 460), lock_e))
        aperture = int(lerp(146, 22, purge_e))
        for off, col in [(-60, cyan), (-28, dim_cyan), (28, dim_gold), (60, gold)]:
            cv.create_line(cx - span, cy + off, cx - aperture, cy + off, fill=col, width=1)
            cv.create_line(cx + aperture, cy + off, cx + span, cy + off, fill=col, width=1)

        base_r = int(lerp(34, 194, lock_e * (1.0 - purge_e * 0.20)))
        for extra, col in [(0, cyan), (20, gold)]:
            r = max(22, int((base_r + extra) * (1.0 - 0.58 * purge_e)))
            arm = 18 + extra // 3
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = cx + sx * r
                    py = cy + sy * r
                    cv.create_line(px, py, px - sx * arm, py, fill=col, width=1)
                    cv.create_line(px, py, px, py - sy * arm, fill=col, width=1)

        diamond = int(lerp(24, 9, purge_e))
        cv.create_polygon(cx, cy - diamond, cx + diamond, cy,
                          cx, cy + diamond, cx - diamond, cy,
                          outline=white, fill='')
        cv.create_line(cx - 38, cy, cx + 38, cy, fill=white, width=1)
        cv.create_line(cx, cy - 18, cx, cy + 18, fill=white, width=1)

        if purge_t > 0.0 and not pulse_gl_drawn:
            burst = int(lerp(18, 220, purge_e))
            flash = '#d7f7ff' if purge_t < 0.7 else gold
            cv.create_line(cx - burst, cy, cx + burst, cy, fill=flash, width=2)
            cv.create_line(cx, cy - int(burst * 0.42), cx, cy + int(burst * 0.42), fill=flash, width=1)

        if tv_fade < 0.92:
            scan_y = int(lerp(cy - 140, cy + 120, sweep))
            cv.create_line(max(0, cx - span - 140), scan_y,
                           min(sw, cx + span + 140), scan_y,
                           fill=cyan, width=1)
            cv.create_line(max(0, cx - span - 120), scan_y + 3,
                           min(sw, cx + span + 120), scan_y + 3,
                           fill=dim_cyan, width=1)

        banner_x1 = max(30, cx - span - 60)
        banner_x2 = min(sw - 30, cx + span + 60)
        seq_label = 'SEQ:SHIFT' if ov.get('mode') == 'switch' else 'SEQ:EXIT'
        status_label = 'STATUS:LOCK' if purge_t < 0.08 else ('STATUS:TRANSFER' if ov.get('mode') == 'switch' else 'STATUS:PURGE')
        cv.create_text(banner_x1, max(24, cy - 150), text='SYS:ENTITY',
                       anchor='w', fill=cyan, font=('Consolas', 9))
        cv.create_text(banner_x2, max(24, cy - 150), text=seq_label,
                       anchor='e', fill=gold, font=('Consolas', 9))
        cv.create_text(banner_x1, min(sh - 24, cy + 164), text=status_label,
                       anchor='w', fill=dim_cyan, font=('Consolas', 9))
        cv.create_text(banner_x2, min(sh - 24, cy + 164), text=time.strftime('%H:%M:%S'),
                       anchor='e', fill=dim_gold, font=('Consolas', 9))

        text_y = cy + 86
        primary = 'ENTITY LOCK' if purge_t < 0.12 else ov['banner']['primary']
        tertiary = 'FREEZING UI STATE' if purge_t < 0.12 else ov['banner']['tertiary']
        cv.create_text(cx, text_y, text=primary,
                       fill=white, font=get_sao_font(16, True))
        cv.create_text(cx, text_y + 26, text=ov['banner']['secondary'],
                       fill=gold, font=('Consolas', 11, 'bold'))
        cv.create_text(cx, text_y + 48, text=tertiary,
                       fill='#8aaec0', font=('Consolas', 9))

    def _collect_exit_windows(self):
        wins = []
        seen = set()

        try:
            focus_x = self._float.winfo_x() + self._fw // 2
            focus_y = self._float.winfo_y() + self._fh // 2
        except Exception:
            focus_x = self.root.winfo_screenwidth() // 2
            focus_y = self.root.winfo_screenheight() // 2

        def _profile(x, y, role, order):
            dx = x - focus_x
            dy = y - focus_y
            dist = max(1.0, math.hypot(dx, dy))
            ux, uy = dx / dist, dy / dist
            if role == 'float':
                return {'delay': 0.28, 'duration': 0.52, 'travel': 86,
                        'ux': 1.0, 'uy': -0.25, 'movable': True}
            if role == 'panel':
                return {'delay': 0.12 + order * 0.085, 'duration': 0.40,
                        'travel': 48 + order * 12, 'ux': ux, 'uy': uy + 0.24, 'movable': True}
            if role == 'menu':
                return {'delay': 0.00, 'duration': 0.32, 'travel': 0,
                        'ux': 0.0, 'uy': 0.0, 'movable': False}
            if role == 'fisheye':
                return {'delay': 0.00, 'duration': 0.24, 'travel': 0,
                        'ux': 0.0, 'uy': 0.0, 'movable': False}
            return {'delay': 0.06, 'duration': 0.32, 'travel': 22,
                    'ux': ux, 'uy': uy, 'movable': True}

        def _add(win, role, order=0, ulw=False):
            if not win:
                return
            try:
                if not win.winfo_exists():
                    return
                wid = win.winfo_id()
                if wid in seen:
                    return
                seen.add(wid)
                try:
                    alpha = float(win.attributes('-alpha'))
                except Exception:
                    alpha = 1.0
                profile = _profile(win.winfo_x(), win.winfo_y(), role, order)
                wins.append({
                    'win': win,
                    'alpha': max(0.0, min(1.0, alpha)),
                    'x': win.winfo_x(),
                    'y': win.winfo_y(),
                    'role': role,
                    'ulw': ulw,
                    **profile,
                })
            except Exception:
                pass

        # HP float 使用 ULW，不能用 attributes('-alpha') 读写
        _float = getattr(self, '_float', None)
        if _float:
            try:
                if _float.winfo_exists():
                    wid = _float.winfo_id()
                    if wid not in seen:
                        seen.add(wid)
                        profile = _profile(_float.winfo_x(), _float.winfo_y(), 'float', 0)
                        wins.append({
                            'win': _float,
                            'alpha': getattr(self, '_float_alpha', 1.0),
                            'x': _float.winfo_x(),
                            'y': _float.winfo_y(),
                            'role': 'float',
                            'ulw': True,
                            **profile,
                        })
            except Exception:
                pass
        for idx, panel in enumerate([self._piano_panel, self._viz_panel, self._status_panel, self._control_panel]):
            _add(panel, 'panel', order=idx)
        _add(getattr(getattr(self, '_sao_menu', None), '_overlay', None), 'menu')
        _add(getattr(self, '_fisheye_ov', None), 'fisheye')
        # _hp_alpha_windows 已废弃 (ULW 内部渲染)
        return wins

    def _finalize_close(self):
        if self._close_finalized:
            return
        self._close_finalized = True
        self._destroyed = True
        self._breath_active = False
        self._lift_loop_active = False
        self._cleanup_entry_overlay()
        self._cleanup_exit_overlay()
        if hasattr(self, '_hotkey_mgr'):
            self._hotkey_mgr.cleanup()
        self._stop_fisheye_overlay()
        try:
            self._sao_menu.unbind_events()
            if self._sao_menu.visible:
                self._sao_menu.close()
        except Exception:
            pass
        # 停止识别引擎
        self._recognition_active = False
        self._stop_recognition_engines()
        # 保存缓存
        if self._state_mgr and self._cfg_settings_ref:
            try: self._state_mgr.save_cache(self._cfg_settings_ref)
            except Exception: pass
        # 销毁所有浮动面板
        for panel in [self._piano_panel, self._viz_panel, self._status_panel, self._control_panel]:
            try:
                if panel and panel.winfo_exists():
                    panel.destroy()
            except Exception:
                pass
        # 销毁 ULW 覆盖层 + 配置面板
        for ov in [self._dps_overlay, self._boss_hp_overlay, self._alert_overlay]:
            try:
                if ov:
                    ov.destroy()
            except Exception:
                pass
        for pnl in [self._autokey_panel, self._bossraid_panel]:
            try:
                if pnl:
                    pnl.destroy()
            except Exception:
                pass
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._alert_overlay = None
        self._autokey_panel = None
        self._bossraid_panel = None
        self._destroy_hp_alpha_strip_windows()
        try:
            if self._float and self._float.winfo_exists():
                self._float.destroy()
        except Exception:
            pass
        try:
            self.root.quit()  # 退出 mainloop，由 run() 负责 destroy
        except Exception:
            pass

    def _run_exit_animation(self, after_shutdown=None, mode='exit', target_label=None):
        if self._close_finalized or self._exit_animating:
            return
        self._exit_animating = True
        self._destroyed = True
        self._breath_active = False
        self._lift_loop_active = False
        try:
            play_sound('menu_close')
        except Exception:
            pass
        try:
            self._play_motion_blur(closing=True)
        except Exception:
            pass
        try:
            if self._sao_menu.visible:
                self._sao_menu.close()
        except Exception:
            pass

        wins = self._collect_exit_windows()
        self._create_exit_overlay(mode=mode, target_label=target_label)
        if not wins:
            self._draw_exit_overlay(1.0)
            self._finalize_close()
            if after_shutdown:
                try:
                    after_shutdown()
                except Exception:
                    pass
            return

        t0 = time.time()
        stage1 = 0.42
        stage2 = 0.96
        duration = stage1 + stage2

        def _finish():
            self._finalize_close()
            if after_shutdown:
                try:
                    after_shutdown()
                except Exception:
                    pass

        def _step():
            if self._close_finalized:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / duration)
            self._draw_exit_overlay(t)
            for item in wins:
                try:
                    win = item['win']
                    if not win.winfo_exists():
                        continue
                    if elapsed < stage1:
                        hold = ease_out(min(1.0, elapsed / stage1))
                        new_alpha = item['alpha'] * (1.0 - 0.16 * hold)
                        if item.get('movable'):
                            dx = int(item['ux'] * item['travel'] * 0.10 * hold)
                            dy = int(item['uy'] * item['travel'] * 0.10 * hold)
                            if item.get('role') == 'float':
                                dy -= int(8 * hold)
                            try:
                                win.geometry(f'+{item["x"] + dx}+{item["y"] + dy}')
                            except Exception:
                                pass
                    else:
                        local = min(1.0, max(0.0, (elapsed - stage1 - item['delay']) / max(0.001, item['duration'])))
                        fade = ease_in_out(local)
                        base_alpha = item['alpha'] * 0.84
                        new_alpha = max(0.0, base_alpha * (1.0 - fade))
                        if item.get('movable'):
                            dx = int(item['ux'] * item['travel'] * (0.10 + 0.90 * fade))
                            dy = int(item['uy'] * item['travel'] * (0.10 + 0.90 * fade))
                            if item.get('role') == 'float':
                                dy -= int(18 + 18 * fade)
                            try:
                                win.geometry(f'+{item["x"] + dx}+{item["y"] + dy}')
                            except Exception:
                                pass
                    if item.get('ulw'):
                        self._set_float_alpha(new_alpha)
                    else:
                        win.attributes('-alpha', new_alpha)
                except Exception:
                    pass
            if elapsed < duration:
                try:
                    self.root.after(16, _step)
                except Exception:
                    _finish()
            else:
                _finish()

        try:
            self.root.after(1, _step)
        except Exception:
            _finish()

    def _on_close(self):
        self._run_exit_animation(mode='exit', target_label='Desktop')

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()
        # mainloop 已退出 — 清理 root 并处理热切换
        try:
            self.root.destroy()
        except Exception:
            pass
        if hasattr(self, '_after_shutdown') and self._after_shutdown:
            cb = self._after_shutdown
            self._after_shutdown = None
            try:
                cb()
            except Exception as e:
                print(f"[SAO] Hot switch: {e}")
                import traceback; traceback.print_exc()


def main():
    # 设置 DPI 感知 — 减少 Tkinter 控件锯齿 / 模糊
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
    app = SAOPlayerGUI()
    app.run()


if __name__ == "__main__":
    main()
