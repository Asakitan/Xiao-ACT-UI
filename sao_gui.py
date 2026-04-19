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
from render_capture_sync import wait_until_capture_idle

from config import (
    APP_VERSION_LABEL, WINDOW_TITLE, WINDOW_SIZE,
    DEFAULT_HOTKEYS,
    FONTS_DIR,
    SettingsManager,
    get_skill_slot_rects,
    resource_path,
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
from sao_gui_hp import HpOverlay
from sao_gui_alert import AlertOverlay
from sao_gui_skillfx import BurstReadyOverlay
from sao_gui_autokey import AutoKeyPanel
from sao_gui_bossraid import BossRaidPanel
from sao_gui_commander import CommanderPanel
from sao_gui_profile_editors import AutoKeyDetailPanel, BossRaidDetailPanel

try:
    import pynput.keyboard as pynput_kb
    from pynput.keyboard import Key, KeyCode
    PYNPUT_HOTKEY_AVAILABLE = True
except Exception:
    pynput_kb = None
    Key = None
    KeyCode = None
    PYNPUT_HOTKEY_AVAILABLE = False


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
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'settings.json')

# pyglet Link Start 渲染器 (已弃用, 保留文件但不再使用)
# OpenGL 上下文要求主线程, 与 tkinter 冲突, 改用 Canvas SAO-UI 隧道模型
HAS_PYGLET = False

# ── 全局快捷键检测 (复用 gui.py 逻辑) ──
def _is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
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
    _LEGACY_KEYS = (
        'last_file', 'speed', 'transpose', 'chord_mode',
        'show_piano', 'show_viz', 'show_control',
        'piano_x', 'piano_y', 'viz_x', 'viz_y',
        'control_x', 'control_y',
    )

    def __init__(self):
        self.settings = {
            'hotkeys': DEFAULT_HOTKEYS.copy(),
            'ui_mode': 'entity',
        }
        self.load()

    def _prune_legacy_settings(self):
        dirty = False
        for legacy_key in self._LEGACY_KEYS:
            if legacy_key in self.settings:
                self.settings.pop(legacy_key, None)
                dirty = True
        return dirty

    def load(self):
        dirty = False
        try:
            if os.path.exists(CONFIG_FILE):
                with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
                    self.settings.update(json.load(f))
        except:
            pass
        if self.settings.get('ui_mode') == 'sao':
            self.settings['ui_mode'] = 'entity'
            dirty = True
        if self._prune_legacy_settings():
            dirty = True
        if dirty:
            self.save()

    def save(self):
        try:
            self._prune_legacy_settings()
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
    p = resource_path('icon.ico')
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
    if not wait_until_capture_idle(0.010):
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
    fname = 'SAOUI.ttf' if family == 'sao' else 'ZhuZiAYuanJWD.ttf'
    fp = os.path.join(FONTS_DIR, fname)
    try:
        font = ImageFont.truetype(fp, size=size)
    except Exception:
        font = ImageFont.load_default()
    _cache[key] = font
    return font


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
    - Top 区 (白色, 240×280): 用户名/分隔线/等级/EXP/HP/STA
    - Bottom 区 (灰色, 240×120): 菜单模式状态
    - 右三角指示器 (连接 MenuBar)
    - 下三角装饰 (连接 top/bottom)
    """

    def __init__(self, parent, username='Player', profession='', **kw):
        # 不继承 parent bg (#010101 = 透明色键), 用实际可见色
        super().__init__(parent, bg='#ffffff', highlightthickness=0, **kw)
        self._active = False
        self._anim = Animator(self)
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

    def update_shift_mode(self, mode_text: str):
        self._shift_mode = mode_text
        if self._active:
            self._redraw_bottom(self._target_w, self._bottom_h)
        if self._on_mode_change:
            try:
                self._on_mode_change(mode_text)
            except Exception:
                pass

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
            t = time.time()
            scan_x = 10 + int((w - 20) * ((math.sin(t * 1.5) + 1) / 2))
            self._top.create_rectangle(scan_x - 12, scan_y - 1,
                                       scan_x + 12, scan_y + 1,
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



# ══════════════════════════════════════════════════════════
#  SAO Player GUI — 纯悬浮 SAO Menu 架构
# ══════════════════════════════════════════════════════════
class SAOPlayerGUI:
    """
    纯悬浮 SAO Utils 风格 GUI — 没有传统窗口！
    - 常驻: 小型悬浮触发按钮 (Toplevel)
    - 展开: SAO PopUpMenu 全屏菜单 = 主界面
    - 左面板: SAOPlayerPanel (玩家信息/赛季进度/状态)
    - 菜单按钮: 5 类 (控制/自动/Boss/面板/关于)
    - 子菜单: 实时工具与面板控制
    - 可选: 浮动钢琴/可视化面板
    """

    def __init__(self):
        _set_process_app_id('sao.auto.game.ui')
        self.root = tk.Tk()
        self.root.withdraw()  # root 永远隐藏, 只作为 Tk 事件循环
        self.root.title("SAO Auto — 游戏辅助 UI")

        self.settings = SettingsManager()
        # 记录当前 UI 模式 — 下次启动时使用
        self.settings.set('ui_mode', 'entity')
        self.settings.save()

        # ── 角色配置 ──
        profile = load_profile()
        self._username = profile.get('username', '')
        self._profession = profile.get('profession', '')
        self._level = profile.get('level', 1)
        self._level_extra = 0
        self._season_exp = 0
        self._sta_offline_armed = False

        # 加载 SAO 字体
        load_sao_fonts()

        self._current_file = None
        self._panels_hidden = False  # 一键隐藏所有面板
        self._hidden_panels_snapshot = []  # 隐藏前记录哪些面板是开的
        self._player_panel = None  # 当 SAO 菜单打开时设置
        self._picker = None        # SAOFilePicker 引用 (防止 GC)
        self._status_panel = None  # 浮动状态面板
        self._update_panel = None  # 浮动更新面板
        self._update_snapshot = None
        self._update_listener_installed = False
        self._update_listener = None
        self._update_panel_hidden = False
        self._update_panel_state_key = ''
        self._update_popup_ready = False
        self._pending_update_popup_snapshot = None
        self._last_update_popup_key = ''
        self._menu_refresh_after_id = None
        self._menu_refresh_force = False
        self._menu_children_cache_sig = None
        self._menu_children_cache = None
        self._fisheye_ov = None    # 菜单开启时的持久鱼眼叠加层
        self._ctx_menu_open = False  # 右键菜单弹出中, 暂停 z-order 置顶
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
        self._dps_visible = False
        self._dps_enabled = True
        self._dps_faded = False
        self._dps_mode = 'hidden'
        self._dps_last_report_available = False
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
        self._last_fast_state_sig = None

        # ── Boss bar target tracking (mirrors webview) ──
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._bb_damage_timeout = 15.0
        self._hide_seek_engine = None
        self._hide_seek_alert_timer = None
        self._hide_seek_alert_active = False

        # ── ULW 覆盖层引用 ──
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._hp_overlay = None
        self._alert_overlay = None
        self._skillfx_overlay = None
        self._skillfx_layout = None

        # ── 配置面板实例 ──
        self._autokey_panel = None    # AutoKeyPanel
        self._bossraid_panel = None   # BossRaidPanel
        self._autokey_detail_panel = None
        self._bossraid_detail_panel = None
        self._commander_panel = None  # CommanderPanel
        self._commander_last_push = 0.0

        self._set_icon()
        self._create_floating_widget()
        self._setup_sao_menu()
        self._setup_hotkeys()
        self.root.after(0, self._ensure_updater_listener)

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
        """(deprecated) HP 外壳已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _render_hp_dynamic(self):
        """(deprecated) HP 动态内容已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _refresh_hp_layered(self):
        """(deprecated) 旧 75%屏宽 HP ULW 已移除, 此方法为兼容占位。"""
        return

    def _set_float_alpha(self, alpha):
        """(deprecated) _float 现为全透明点击锚点, 不再需要 alpha。"""
        self._float_alpha = alpha

    def _animate_float_hud(self):
        """(deprecated) 旧 30fps HP 重绘循环已移除 (HpOverlay 自管帧率)。"""
        return

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
        """SAO 菜单点击锚点窗口 (不渲染 HP — HP 由 sao_gui_hp.HpOverlay 独立渲染).

        历史: 本窗口曾以 UpdateLayeredWindow + PIL 方式渲染一个 75% 屏宽
        的 HP HUD。该渲染已被 ``sao_gui_hp.HpOverlay`` 取代, 故本窗口现
        仅作为 SAOPopUpMenu 的 anchor_widget 和右键菜单宿主使用, 全透
        明但保留点击命中测试。
        """
        try:
            _sw = self.root.winfo_screenwidth()
            _sh = self.root.winfo_screenheight()
        except Exception:
            _sw, _sh = 1920, 1080

        # ── 锚点窗口尺寸 (保持旧 SAO 菜单定位): 75% 屏宽, 高 140px ──
        FW = int(_sw * 0.75)
        FH = 140
        self._fw, self._fh = FW, FH
        self._float_alpha = 0.0
        self._hp_hover = False

        # (legacy — no longer rendered here; kept for stub compatibility)
        self._hp_ox = int(FW * 0.44)
        self._hp_oy = 38
        self._id_plate_w = int(FW * 0.40)

        self._float = tk.Toplevel(self.root)
        self._float.overrideredirect(True)
        self._float.attributes('-topmost', True)
        self._float.geometry(f'{FW}x{FH}')
        # 完全透明点击锚点: Tk 的 -alpha=0.0 (底层 LWA_ALPHA=0) 在 Windows
        # 下窗口不可见, 但 WS_EX_LAYERED + 统一 alpha 模式下点击仍能命中
        # 窗口矩形 — 这正是我们希望的 (保留 SAO 菜单 anchor + 右键菜单).
        self._float.configure(bg='#000000')
        try:
            self._float.attributes('-alpha', 0.0)
        except Exception:
            pass
        _apply_window_icon(self._float)

        # ── 获取 HWND 仅用于 AppBar 样式 (不再用 ULW) ──
        self._float_hwnd = 0
        try:
            self._float.update_idletasks()
            GWL_EXSTYLE = -20
            WS_EX_APPWINDOW = 0x00040000
            WS_EX_TOOLWINDOW = 0x00000080
            hwnd = int(_user32.GetParent(ctypes.c_void_p(self._float.winfo_id())))
            self._float_hwnd = hwnd
            style = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
            style = (style | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW
            _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE, style)
            _disable_native_window_shadow(self._float)
            try:
                _user32.SetWindowDisplayAffinity(ctypes.c_void_p(hwnd), 0x00000011)
            except Exception:
                pass
        except Exception:
            self._float_hwnd = 0

        # (占位符 — 旧 HP shell 缓存, 已弃用)
        self._hp_shell_normal = None
        self._hp_shell_hover = None

        # ── HP 布局常量 (保留 — 外部代码可能仍引用) ──
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
        self._float_ctx.add_command(label='⚡ AutoKey Quick', command=self._toggle_autokey_panel)
        self._float_ctx.add_command(label='⚡ AutoKey Detail', command=self._toggle_autokey_detail_panel)
        self._float_ctx.add_command(label='⚔ BossRaid Quick', command=self._toggle_bossraid_panel)
        self._float_ctx.add_command(label='⚔ BossRaid Detail', command=self._toggle_bossraid_detail_panel)
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
        self._hide_seek_alert_active = False
        timer = getattr(self, '_hide_seek_alert_timer', None)
        if timer:
            try: timer.cancel()
            except Exception: pass
            self._hide_seek_alert_timer = None
        if getattr(self, '_hide_seek_engine', None):
            try: self._hide_seek_engine.stop()
            except Exception: pass
            self._hide_seek_engine = None
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
        self._reset_sta_offline_state()

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
        # Track self→monster damage for boss bar target (mirrors webview)
        if event.get('attacker_is_self') and event.get('target_is_monster'):
            target_uuid = event.get('target_uuid', 0)
            if target_uuid:
                import time as _t
                self._bb_recent_targets[target_uuid] = _t.time()
                self._bb_last_target_uuid = target_uuid
                self._bb_last_damage_ts = _t.time()
                if self._dps_tracker:
                    try: self._dps_tracker.set_boss_uuid(target_uuid)
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
            try:
                self._state_mgr.subscribe(self._on_game_state_update)
            except Exception:
                pass

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
            try:
                self._auto_key_engine.set_burst_actions(self._load_autokey_burst_actions())
            except Exception:
                pass

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
            # DPS panel mirrors webview: stays hidden on startup regardless of
            # the `dps_enabled` setting. Combat/report triggers bring it up.
            self._dps_enabled = bool(self._get_setting('dps_enabled', True))
            self._dps_visible = False
            try:
                self._dps_overlay = DpsOverlay(
                    self.root,
                    self._cfg_settings_ref,
                    request_live_snapshot=self._request_dps_live_snapshot,
                    show_last_report=self._request_dps_last_report,
                    reset_dps=self._reset_dps_tracker,
                    has_last_report=self._get_dps_last_report_available,
                    request_entity_detail=self._request_dps_entity_detail,
                    alert=self._show_entity_alert,
                )
                self._dps_overlay.set_report_available(
                    self._get_dps_last_report_available())
                print('[SAO Entity] DPS overlay initialized (hidden)')
            except Exception as e:
                print(f'[SAO Entity] DPS overlay init failed: {e}')
                self._dps_overlay = None

            try:
                self._boss_hp_overlay = BossHpOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Boss HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Boss HP overlay init failed: {e}')
                self._boss_hp_overlay = None

            self._hp_ov_visible = bool(self._get_setting('hp_ov_enabled', True))
            try:
                self._hp_overlay = HpOverlay(
                    self.root, self._cfg_settings_ref,
                    on_click=self._hp_overlay_on_click,
                    on_menu=self._hp_overlay_on_menu,
                )
                if self._hp_ov_visible:
                    self._hp_overlay.show()
                # Push cached game state to HP overlay immediately
                if self._state_mgr:
                    try:
                        _gs = self._state_mgr.state
                        if _gs.hp_max > 0:
                            _hp_lv = int(_gs.level_base or 1)
                            _hp_lv_extra = int(getattr(_gs, 'level_extra', 0) or 0)
                            if _hp_lv_extra > 0 and _hp_lv > 0:
                                _hp_lv_text = f'{_hp_lv}(+{_hp_lv_extra})'
                            else:
                                _hp_lv_text = str(_hp_lv)
                            self._hp_overlay.update_hp(
                                _gs.hp_current, _gs.hp_max, _hp_lv_text)
                        if _gs.stamina_max > 0:
                            self._hp_overlay.update_sta(
                                _gs.stamina_current, _gs.stamina_max)
                        self._hp_overlay.set_sta_offline(
                            self._should_show_sta_offline(_gs)
                        )
                        if _gs.player_name:
                            self._hp_overlay.set_player_info({
                                'name': _gs.player_name,
                                'profession': _gs.profession_name or '',
                                'uid': _gs.player_id or '',
                            })
                    except Exception:
                        pass
                print('[SAO Entity] Player HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Player HP overlay init failed: {e}')
                self._hp_overlay = None

            try:
                self._alert_overlay = AlertOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Alert overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Alert overlay init failed: {e}')
                self._alert_overlay = None

            try:
                self._skillfx_overlay = BurstReadyOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] SkillFX (Burst) overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] SkillFX overlay init failed: {e}')
                self._skillfx_overlay = None

            # 启动定时缓存保存 (每30秒)
            import threading as _thr
            def _cache_loop():
                import time as _t
                while True:
                    _t.sleep(30)
                    try:
                        self._persist_cached_identity_state(save_now=False)
                        self._state_mgr.save_cache(self._cfg_settings_ref)
                    except Exception:
                        pass
            _thr.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()

        except Exception as e:
            print(f'[SAO Entity] Data engine failed: {e}')
            import traceback; traceback.print_exc()
            self._recognition_active = False

    # ────────────────────────────────────────────
    #  SkillFX / Burst Mode Ready helpers
    # ────────────────────────────────────────────

    def _get_skillfx_layout(self, gs=None):
        """Compute the screen-relative layout for the BurstReady overlay.

        Mirrors the one in sao_webview._get_skillfx_layout so the tk
        port's anchor + callout positioning matches the original webview.
        """
        if gs is None and getattr(self, '_state_mgr', None) is not None:
            gs = self._state_mgr.state
        client_rect = getattr(gs, 'window_rect', None) if gs else None
        if not client_rect:
            try:
                from window_locator import WindowLocator
                client_rect = WindowLocator().get_rect()
            except Exception:
                client_rect = None
        if not client_rect:
            return None
        client_left, client_top, client_right, client_bottom = client_rect
        client_w = max(1, int(client_right - client_left))
        client_h = max(1, int(client_bottom - client_top))

        slots = []
        for slot in list(getattr(gs, 'skill_slots', []) or []) if gs else []:
            if not isinstance(slot, dict):
                continue
            rect = slot.get('rect') or {}
            try:
                sx = int(rect.get('x', 0)); sy = int(rect.get('y', 0))
                sw = int(rect.get('w', 0)); sh = int(rect.get('h', 0))
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slots.append({
                'index': idx,
                'screen_rect': {'x': client_left + sx, 'y': client_top + sy,
                                'w': sw, 'h': sh},
            })
        if not slots:
            for item in get_skill_slot_rects(client_rect):
                left, top, right, bottom = item['bbox']
                slots.append({
                    'index': int(item['index']),
                    'screen_rect': {'x': left, 'y': top,
                                    'w': right - left, 'h': bottom - top},
                })
        if not slots:
            return None

        min_x = min(s['screen_rect']['x'] for s in slots)
        max_y = max(s['screen_rect']['y'] + s['screen_rect']['h'] for s in slots)
        pad_x = max(18, int(round(client_w * 0.012)))
        pad_y = max(18, int(round(client_h * 0.016)))
        pad_left = max(96, int(round(client_w * 0.055)))
        pad_right = max(84, int(round(client_w * 0.044)))
        win_x = max(0, min_x - pad_left)
        win_y = max(0, client_top)
        width = max(420, int((client_right - win_x) + pad_right))
        height = max(220, int((max_y - win_y) + pad_y))
        callout_w = max(440, int(round(client_w * 0.29)))
        callout_h = max(128, int(round(client_h * 0.115)))
        callout_margin_x = max(28, int(round(client_w * 0.022)))
        callout_margin_y = max(24, int(round(client_h * 0.040)))
        callout_x = max(callout_margin_x,
                        width - callout_w - callout_margin_x)
        callout_y = callout_margin_y

        payload_slots = []
        for s in slots:
            r = s['screen_rect']
            payload_slots.append({
                'index': s['index'],
                'rect': {'x': r['x'] - win_x, 'y': r['y'] - win_y,
                         'w': r['w'], 'h': r['h']},
            })
        payload_slots.sort(key=lambda it: it['index'])
        return {
            'window': {'x': int(win_x), 'y': int(win_y),
                       'w': int(width), 'h': int(height)},
            'viewport': {
                'width': int(width), 'height': int(height),
                'padding_x': int(max(pad_x, pad_left, pad_right)),
                'padding_y': int(pad_y),
                'callout': {'x': int(callout_x), 'y': int(callout_y),
                            'w': int(callout_w), 'h': int(callout_h)},
            },
            'slots': payload_slots,
        }

    def _pick_burst_trigger_slot(self, gs):
        watched = self._get_setting(
            'watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        try:
            watched = [int(x) for x in watched if int(x) > 0]
        except Exception:
            watched = []
        if not watched:
            watched = [1]
        slots = getattr(gs, 'skill_slots', []) or []
        edge_slot = 0; first_ready = 0; first_active = 0; first_low_cd = 0
        prev_slot = getattr(self, '_last_burst_slot', 0)
        prev_still_ok = False
        for slot in slots:
            if not isinstance(slot, dict):
                continue
            try:
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx not in watched:
                continue
            state = str(slot.get('state', '') or '').strip().lower()
            try:
                cd = float(slot.get('cooldown_pct', 1.0) or 1.0)
            except Exception:
                cd = 1.0
            is_ready = state in ('ready', 'active') or cd <= 0.02
            if bool(slot.get('ready_edge')) and not edge_slot:
                edge_slot = idx
            if state == 'ready' and not first_ready:
                first_ready = idx
            if state == 'active' and not first_active:
                first_active = idx
            if cd <= 0.02 and not first_low_cd:
                first_low_cd = idx
            if idx == prev_slot and is_ready:
                prev_still_ok = True
        if edge_slot:
            chosen = edge_slot
        elif prev_still_ok and prev_slot:
            chosen = prev_slot
        elif first_ready:
            chosen = first_ready
        elif first_active:
            chosen = first_active
        elif first_low_cd:
            chosen = first_low_cd
        else:
            chosen = 0
        self._last_burst_slot = chosen
        return chosen

    def _format_level_text(self, level_base: int, level_extra: int) -> str:
        level_base = int(level_base or self._level or 1)
        level_extra = int(level_extra or 0)
        if level_extra > 0 and level_base > 0:
            return f'{level_base}(+{level_extra})'
        return str(level_base)

    def _reset_sta_offline_state(self):
        self._sta_offline_armed = False
        try:
            if self._hp_overlay and getattr(self, '_hp_ov_visible', True):
                self._hp_overlay.set_sta_offline(False)
        except Exception:
            pass

    def _should_show_sta_offline(self, gs) -> bool:
        if gs is None:
            return False
        # STA OFFLINE 仅由 vision 驱动。packet 活动不会抹除该状态，
        # 但也不会因为 vision recognition_ok 闪动到 False 而误报。
        return bool(getattr(gs, 'stamina_offline', False))

    def _persist_cached_identity_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        cache = dict(settings.get('game_cache', {}) or {})
        name = str(getattr(self, '_username', '') or '').strip()
        profession = str(getattr(self, '_profession', '') or '').strip()
        level_base = int(getattr(self, '_level', 0) or 0)
        level_extra = int(getattr(self, '_level_extra', 0) or 0)
        season_exp = int(getattr(self, '_season_exp', 0) or 0)
        if name:
            cache['player_name'] = name
        if profession:
            cache['profession_name'] = profession
        if level_base > 0:
            cache['level_base'] = level_base
        if level_extra > 0:
            cache['level_extra'] = level_extra
        if season_exp > 0:
            cache['season_exp'] = season_exp
        gs = getattr(self, '_game_state', None)
        uid = str(getattr(gs, 'player_id', '') or '').strip() if gs is not None else ''
        if uid:
            cache['player_id'] = uid
        settings.set('game_cache', cache)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

    def _persist_entity_menu_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        active_name = ''
        try:
            menu_bar = getattr(getattr(self, '_sao_menu', None), '_menu_bar', None)
            active_item = getattr(menu_bar, '_active_item', None)
            if isinstance(active_item, dict):
                active_name = str(active_item.get('name') or '').strip()
        except Exception:
            active_name = ''
        settings.set('entity_last_menu', active_name)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

    def _restore_entity_menu_state(self):
        saved_name = str(self._get_setting('entity_last_menu', '') or '').strip()
        if not saved_name:
            return
        menu = getattr(self, '_sao_menu', None)
        menu_bar = getattr(menu, '_menu_bar', None)
        if not menu or not menu.visible or menu_bar is None:
            return
        active_item = getattr(menu_bar, '_active_item', None)
        if isinstance(active_item, dict) and str(active_item.get('name') or '').strip() == saved_name:
            return
        item = next((it for it in (self._menu_icons or []) if str(it.get('name') or '').strip() == saved_name), None)
        if item:
            menu_bar._on_item_click(item)

    def _on_game_state_update(self, gs):
        """Fast path for identity/level updates from packet or vision threads."""
        if self._destroyed or gs is None:
            return
        try:
            sig = (
                int(getattr(gs, 'level_base', 0) or 0),
                int(getattr(gs, 'level_extra', 0) or 0),
                int(getattr(gs, 'season_exp', 0) or 0),
                str(getattr(gs, 'player_name', '') or ''),
                str(getattr(gs, 'profession_name', '') or ''),
                str(getattr(gs, 'player_id', '') or ''),
            )
        except Exception:
            return
        if sig == getattr(self, '_last_fast_state_sig', None):
            return
        self._last_fast_state_sig = sig
        try:
            self.root.after(0, lambda snap=gs: self._apply_fast_state_update(snap))
        except Exception:
            pass

    def _apply_fast_state_update(self, gs):
        if self._destroyed or gs is None:
            return
        try:
            level_base = int(getattr(gs, 'level_base', 0) or self._level or 1)
            level_extra = int(getattr(gs, 'level_extra', 0) or 0)
            season_exp = int(getattr(gs, 'season_exp', 0) or 0)
            self._level = max(1, level_base)
            self._level_extra = max(0, level_extra)
            self._season_exp = max(0, season_exp)

            player_name = str(getattr(gs, 'player_name', '') or '')
            profession = str(getattr(gs, 'profession_name', '') or '')
            uid = str(getattr(gs, 'player_id', '') or '')
            if player_name:
                self._username = player_name
                disp = player_name
                if len(disp) > 10:
                    disp = disp[:9] + '...'
                self._hp_display_name = disp
            if profession:
                self._profession = profession
            if self._sao_menu:
                self._sao_menu.username = self._username or 'Player'
                self._sao_menu.description = self._profession or 'SAO Auto'

            panel = getattr(self, '_player_panel', None)
            if panel:
                try:
                    panel.update_level(self._level, self._level_extra, self._season_exp)
                except Exception:
                    pass

            if self._hp_overlay and getattr(self, '_hp_ov_visible', True):
                hp = int(getattr(gs, 'hp_current', 0) or 0)
                hp_max = int(getattr(gs, 'hp_max', 0) or 0)
                if hp_max <= 0:
                    hp, hp_max = getattr(self, '_sta_hp', (0, 1))
                self._hp_overlay.update_hp(
                    int(hp), max(1, int(hp_max)),
                    self._format_level_text(self._level, self._level_extra),
                )
                if player_name or profession or uid:
                    self._hp_overlay.set_player_info({
                        'name': player_name or self._username or '',
                        'profession': profession or self._profession or '',
                        'uid': uid,
                    })
        except Exception:
            pass

    def _push_packet_overlays(self, gs):
        """始终运行的 DPS / Boss HP 覆盖板推送 — 数据完全由 packet on_damage 回调驱动,
        与 recognition_ok / packet_active 闸门解耦, 避免抓包链路里任何一处中断都拖累弹出.
        """
        # ── DPS tracker: 更新自身玩家信息 ──
        if self._dps_tracker and gs is not None and getattr(gs, 'player_id', ''):
            try:
                _p_uid = int(gs.player_id) if str(gs.player_id).isdigit() else 0
                if _p_uid:
                    self._dps_tracker.set_self_uid(_p_uid)
                    if self._dps_overlay:
                        self._dps_overlay.set_self_uid(_p_uid)
                    _self_fp = 0
                    _bridge = getattr(self, '_packet_engine', None)
                    if _bridge:
                        _all_p = _bridge.get_players()
                        _sp = _all_p.get(_p_uid)
                        if _sp:
                            _self_fp = getattr(_sp, 'fight_point', 0) or 0
                    self._dps_tracker.update_player_info(
                        _p_uid,
                        gs.player_name or '',
                        gs.profession_name or '',
                        _self_fp,
                        int(gs.level_base or 0),
                    )
            except Exception:
                pass

        # ── 同步队伍中所有玩家信息 ──
        if self._dps_tracker:
            _bridge = getattr(self, '_packet_engine', None)
            if _bridge:
                try:
                    for _pu, _pd in _bridge.get_players().items():
                        if _pu and _pd.name:
                            self._dps_tracker.update_player_info(
                                _pu,
                                _pd.name or '',
                                _pd.profession or '',
                                getattr(_pd, 'fight_point', 0) or 0,
                                getattr(_pd, 'level', 0) or 0,
                            )
                except Exception:
                    pass

        # ── DPS Overlay push ──
        if self._dps_tracker:
            try:
                _dps_enabled = bool(self._get_setting('dps_enabled', True))
                _dps_fade_timeout = float(self._get_setting('dps_fade_timeout_s', 15) or 15)
                _dps_fade_timeout = max(5.0, _dps_fade_timeout)
                self._dps_tracker.finalize_if_idle(_dps_fade_timeout, 'idle_timeout')
                self._sync_dps_report_availability()
                if self._dps_tracker.is_dirty():
                    _dps_snap = self._dps_tracker.get_snapshot()
                    _dps_has_live = bool(
                        int(_dps_snap.get('total_damage') or 0) > 0
                        and self._dps_tracker.has_recent_damage(_dps_fade_timeout)
                    )
                    if self._dps_overlay:
                        self._dps_overlay.set_report_available(
                            self._get_dps_last_report_available())
                    if _dps_enabled and _dps_has_live and self._dps_overlay and self._dps_mode != 'report':
                        if not self._dps_visible:
                            self._dps_visible = True
                            self._dps_faded = False
                            self._dps_mode = 'live'
                            self._dps_overlay.show()
                        self._dps_overlay.update(_dps_snap)
                    elif self._dps_overlay and (self._dps_visible or self._dps_mode == 'report'):
                        self._dps_overlay.update(_dps_snap)
                if (self._dps_overlay
                        and getattr(self._dps_overlay, '_detail_visible', False)
                        and self._dps_mode == 'live'):
                    _detail_uid = int(getattr(self._dps_overlay,
                                              '_detail_uid', 0) or 0)
                    if _detail_uid > 0:
                        try:
                            _det = self._dps_tracker.get_entity_detail(_detail_uid)
                            if _det:
                                self._dps_overlay.update_detail(_det)
                        except Exception:
                            pass
                if self._dps_overlay and self._dps_visible and self._dps_mode != 'report':
                    if not self._dps_tracker.has_recent_damage(_dps_fade_timeout):
                        if not self._dps_faded:
                            self._dps_overlay.fade_out()
                            self._dps_faded = True
                        self._dps_visible = False
                        self._dps_mode = 'hidden'
                    elif self._dps_faded:
                        self._dps_overlay.fade_in()
                        self._dps_faded = False
            except Exception:
                pass

        # ── Boss HP Overlay push (镜像 webview target-based 追踪) ──
        if self._boss_hp_overlay and gs is not None:
            try:
                _bb_raid_active = getattr(gs, 'boss_raid_active', False)
                _bb_mode = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
                _bb_src = getattr(gs, 'boss_hp_source', 'none') or 'none'

                _now = time.time()
                _has_recent_self_damage = (_now - self._bb_last_damage_ts) < self._bb_damage_timeout

                for uuid in list(self._bb_recent_targets.keys()):
                    if _now - self._bb_recent_targets.get(uuid, 0) > self._bb_damage_timeout * 3:
                        self._bb_recent_targets.pop(uuid, None)

                _bb_direct_data = None
                _bb_direct_hp = 0
                _bb_direct_max = 0
                _bridge = getattr(self, '_packet_engine', None)
                if not _bb_raid_active:
                    if _bridge and _has_recent_self_damage and self._bb_recent_targets:
                        _recent_monsters = []
                        for uuid, dmg_ts in list(self._bb_recent_targets.items()):
                            if _now - dmg_ts < self._bb_damage_timeout * 1.5:
                                m = _bridge.get_monster(uuid)
                                if m and not getattr(m, 'is_dead', False) and (getattr(m, 'max_hp', 0) > 0 or getattr(m, 'hp', 0) > 0):
                                    _recent_monsters.append(m)
                        if _recent_monsters:
                            def _sort_key(m):
                                hp = getattr(m, 'hp', 0) or 0
                                maxhp = getattr(m, 'max_hp', 0) or hp or 1
                                hp_pct = hp / maxhp if maxhp > 0 else 0
                                last_ts = self._bb_recent_targets.get(getattr(m, 'uuid', 0), 0)
                                return (-hp_pct, -last_ts)
                            _recent_monsters.sort(key=_sort_key)
                            main_m = _recent_monsters[0]
                            self._bb_last_target_uuid = getattr(main_m, 'uuid', 0)
                            _bb_direct_max = int(getattr(main_m, 'max_hp', 0)) or int(getattr(main_m, 'hp', 0))
                            _bb_direct_hp = max(0, int(getattr(main_m, 'hp', 0)))
                            _bb_direct_data = main_m.to_dict() if hasattr(main_m, 'to_dict') else {}
                            _bb_src = 'packet'
                    elif self._bb_last_target_uuid and not _has_recent_self_damage:
                        try:
                            _m = _bridge.get_monster(self._bb_last_target_uuid) if _bridge else None
                            if _m and not getattr(_m, 'is_dead', False) and (getattr(_m, 'max_hp', 0) > 0 or getattr(_m, 'hp', 0) > 0):
                                _bb_direct_max = int(getattr(_m, 'max_hp', 0)) or int(getattr(_m, 'hp', 0))
                                _bb_direct_hp = max(0, int(getattr(_m, 'hp', 0)))
                                _bb_direct_data = _m.to_dict() if hasattr(_m, 'to_dict') else {}
                                _bb_src = 'packet'
                        except Exception:
                            pass

                if _bb_mode == 'off':
                    _bb_show = False
                elif _bb_raid_active:
                    _bb_show = True
                else:
                    _bb_show = _has_recent_self_damage and (_bb_src != 'none' or _bb_direct_data is not None)

                if _bb_direct_data and not _bb_raid_active:
                    _bb_hp_pct = _bb_direct_hp / _bb_direct_max if _bb_direct_max > 0 else 1.0
                    _bb_data = {
                        'active': _bb_show,
                        'hp_pct': round(_bb_hp_pct, 3),
                        'hp_source': _bb_src,
                        'current_hp': _bb_direct_hp,
                        'total_hp': _bb_direct_max,
                        'shield_active': bool(_bb_direct_data.get('shield_active')),
                        'shield_pct': round(float(_bb_direct_data.get('shield_pct') or 0.0), 3),
                        'breaking_stage': int(_bb_direct_data.get('breaking_stage') or 0),
                        'has_break_data': bool(_bb_direct_data.get('has_break_data')),
                        'extinction_pct': round(float(_bb_direct_data.get('extinction_pct') or 0.0), 3),
                        'extinction': int(_bb_direct_data.get('extinction') or 0),
                        'max_extinction': int(_bb_direct_data.get('max_extinction') or 0),
                        'stop_breaking_ticking': bool(_bb_direct_data.get('stop_breaking_ticking')),
                        'in_overdrive': bool(_bb_direct_data.get('in_overdrive')),
                        'invincible': False,
                        'boss_name': str(_bb_direct_data.get('name', ''))[:20] or '',
                    }
                else:
                    _bb_breaking_stage_gs = getattr(gs, 'boss_breaking_stage', -1)
                    _bb_data = {
                        'active': _bb_show,
                        'hp_pct': round(getattr(gs, 'boss_hp_est_pct', 1.0), 3),
                        'hp_source': _bb_src,
                        'current_hp': getattr(gs, 'boss_current_hp', 0),
                        'total_hp': getattr(gs, 'boss_total_hp', 0),
                        'shield_active': getattr(gs, 'boss_shield_active', False),
                        'shield_pct': round(getattr(gs, 'boss_shield_pct', 0.0), 3),
                        'breaking_stage': _bb_breaking_stage_gs,
                        'has_break_data': _bb_breaking_stage_gs != -1,
                        'extinction_pct': round(getattr(gs, 'boss_extinction_pct', 0.0), 3),
                        'extinction': 0,
                        'max_extinction': 0,
                        'stop_breaking_ticking': False,
                        'in_overdrive': getattr(gs, 'boss_in_overdrive', False),
                        'invincible': getattr(gs, 'boss_invincible', False),
                        'boss_name': '',
                    }
                self._boss_hp_overlay.update(_bb_data)
            except Exception:
                pass

    def _recognition_loop(self):
        """后台识别循环 — 读取 GameStateManager 并更新 HP 条 + 体力覆盖板 + DPS + Boss.

        注意: v2.1.2-f 起完全去除外层 `_recognition_active` 闸门 —
        即使 vision/packet 引擎初始化中途失败 (engines 列表为空导致
        `_recognition_active=False`), 只要 GameStateManager 还在运行,
        就应继续刷新 overlay。子模块 (DPS/Boss HP/BurstReady/HP overlay)
        各自做空数据检查, 不会因为闸门翻成 False 而集体卡死。
        """
        if self._destroyed:
            return
        if self._state_mgr is not None:
            try:
                gs = self._state_mgr.state
                # DPS / Boss HP 推送独立于 recognition gate, 完全由抓包驱动 ——
                # 只要 packet bridge 收到伤害事件就能弹出
                try:
                    self._push_packet_overlays(gs)
                except Exception as _e_pp:
                    if not getattr(self, '_pp_err_logged', False):
                        self._pp_err_logged = True
                        print(f'[SAO Entity] _push_packet_overlays error: {_e_pp}')
                        import traceback as _tb
                        _tb.print_exc()
                # ── 与 webview 对齐: HP/STA、SkillFX、commander、identity 持久化等
                # 均依靠各子模块自带的字段空检查来决定是否更新, 不再被 recognition_ok
                # / packet_active 总闸门拦下, 否则任意一路 vision/packet 闪断都会
                # 让整圈 overlay 卡住 (例如 BurstReady 不出来).
                if True:
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
                    menu_level = int(gs.level_base or self._level or 1)
                    menu_level_extra = int(getattr(gs, 'level_extra', 0) or 0)
                    menu_season_exp = int(getattr(gs, 'season_exp', 0) or 0)
                    self._level = menu_level
                    self._level_extra = menu_level_extra
                    self._season_exp = menu_season_exp

                    # 同步 HP/STA 到菜单面板
                    _pp = getattr(self, '_player_panel', None)
                    if _pp:
                        _pp._sta_hp = (hp, hp_max)
                        _pp._sta_sta = (sta, sta_max)
                        _pp.update_level(menu_level, menu_level_extra, menu_season_exp)

                    # DPS / Boss HP 推送已迁移到 _push_packet_overlays (在闸门外执行)

                    # ── Player HP Overlay push ──
                    if self._hp_overlay and getattr(self, '_hp_ov_visible', True):
                        try:
                            _lv = int(gs.level_base or 1)
                            _lv_extra = int(getattr(gs, 'level_extra', 0) or 0)
                            if _lv_extra > 0 and _lv > 0:
                                _lv_text = f'{_lv}(+{_lv_extra})'
                            else:
                                _lv_text = str(_lv)
                            self._hp_overlay.update_hp(hp, hp_max, _lv_text)
                            self._hp_overlay.update_sta(sta, sta_max)
                            _sta_offline = self._should_show_sta_offline(gs)
                            self._hp_overlay.set_sta_offline(
                                _sta_offline)
                            if gs.player_name:
                                _pi_sig = (
                                    str(gs.player_name),
                                    str(gs.profession_name or ''),
                                    str(gs.player_id or ''),
                                )
                                if _pi_sig != getattr(self, '_last_hp_player_info_sig', None):
                                    self._last_hp_player_info_sig = _pi_sig
                                    self._hp_overlay.set_player_info({
                                        'name': _pi_sig[0],
                                        'profession': _pi_sig[1],
                                        'uid': _pi_sig[2],
                                    })
                            # Boss timer (use boss_timer_text + boss_enrage_remaining like webview)
                            _boss_text = getattr(gs, 'boss_timer_text', '') or ''
                            _boss_active = getattr(gs, 'boss_raid_active', False)
                            _boss_enrage = float(getattr(gs, 'boss_enrage_remaining', 0) or 0)
                            if _boss_active and _boss_text:
                                _boss_urgency = 'urgent' if 0 < _boss_enrage < 60 else 'normal'
                            else:
                                _boss_text = ''
                                _boss_urgency = 'normal'
                            if _boss_text != self._last_boss_timer_text or \
                               _boss_urgency != self._last_boss_timer_urgency:
                                self._last_boss_timer_text = _boss_text
                                self._last_boss_timer_urgency = _boss_urgency
                                self._hp_overlay.set_boss_timer(_boss_text, _boss_urgency)
                        except Exception:
                            pass

                    # ── SkillFX (Burst Mode Ready) Overlay push ──
                    if self._skillfx_overlay is not None:
                        try:
                            _burst_enabled = bool(self._get_setting('burst_enabled', True))
                            _burst_now = bool(getattr(gs, 'burst_ready', False))
                            _burst_prev = bool(getattr(self, '_last_burst_ready', False))
                            _burst_slot = self._pick_burst_trigger_slot(gs) if _burst_enabled else 0
                            _new_layout = self._get_skillfx_layout(gs)
                            if _new_layout and _new_layout != getattr(self, '_skillfx_layout', None):
                                self._skillfx_layout = _new_layout
                                self._skillfx_overlay.set_layout(_new_layout)
                            if _burst_enabled and _burst_now and _burst_slot > 0:
                                if not _burst_prev or _burst_slot != getattr(self, '_last_burst_slot_shown', 0):
                                    self._skillfx_overlay.show_burst(_burst_slot)
                                    self._last_burst_slot_shown = _burst_slot
                            elif _burst_prev and not _burst_now:
                                self._skillfx_overlay.hide_burst()
                                self._last_burst_slot_shown = 0
                            self._last_burst_ready = _burst_now
                        except Exception:
                            pass

                    # ── Commander panel refresh (300 ms) ──
                    try:
                        if self._commander_panel and self._commander_panel.is_visible():
                            import time as _t
                            _now = _t.time()
                            if _now - self._commander_last_push > 0.30:
                                self._commander_last_push = _now
                                self._push_commander_data()
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

        panel.update_level(self._level, self._level_extra, self._season_exp)

        # HP / STA 数据 (来自识别引擎)
        panel._sta_hp = getattr(self, '_sta_hp', (0, 0))
        panel._sta_sta = getattr(self, '_sta_sta', (0, 0))

        # 菜单模式 (从 settings 恢复)
        saved_mode = self._get_setting('shift_mode', '普通模式')
        if saved_mode:
            panel._shift_mode = saved_mode

        # 模式变更 → 自动保存
        panel._on_mode_change = lambda m: self._set_setting('shift_mode', m)

        return panel

    def _compute_menu_refresh_signature(self):
        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS) or {}
        hotkey_sig = tuple(sorted((str(k), str(v)) for k, v in hk.items()))
        try:
            topmost = bool(self._float.attributes('-topmost'))
        except Exception:
            topmost = False

        ak_on = False
        br_on = False
        if self._cfg_settings_ref:
            try:
                ak_on = bool(self._load_auto_key_config().get('enabled', False))
            except Exception:
                ak_on = False
            try:
                br_on = bool(self._load_boss_raid_config().get('enabled', False))
            except Exception:
                br_on = False

        hs_on = bool(self._hide_seek_engine and self._hide_seek_engine.running)
        snd_on = bool(self._get_setting('sound_enabled', True))
        dps_on = bool(self._get_setting('dps_enabled', True))
        dps_report_available = bool(self._get_dps_last_report_available())
        burst_on = bool(self._get_setting('burst_enabled', True))
        burst_slots = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not burst_slots:
            burst_slots = [1]
        boss_bar_mode = str(self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid')
        update_label = self._build_update_menu_label()

        return (
            hotkey_sig,
            bool(getattr(self, '_recognition_active', False)),
            topmost,
            ak_on,
            hs_on,
            br_on,
            snd_on,
            dps_on,
            dps_report_available,
            burst_on,
            tuple(burst_slots),
            boss_bar_mode,
            bool(self._panels_hidden),
            update_label,
        )

    def _get_menu_children_cached(self, force: bool = False):
        sig = self._compute_menu_refresh_signature()
        if not force and self._menu_children_cache is not None and self._menu_children_cache_sig == sig:
            return self._menu_children_cache
        children = self._build_menu_children()
        self._menu_children_cache_sig = sig
        self._menu_children_cache = children
        return children

    def _cancel_pending_menu_refresh(self):
        after_id = getattr(self, '_menu_refresh_after_id', None)
        if after_id:
            try:
                self.root.after_cancel(after_id)
            except Exception:
                pass
        self._menu_refresh_after_id = None
        self._menu_refresh_force = False

    def _apply_menu_refresh_if_open(self):
        self._menu_refresh_after_id = None
        force = bool(self._menu_refresh_force)
        self._menu_refresh_force = False
        if self._destroyed:
            return
        menu = getattr(self, '_sao_menu', None)
        if not (menu and menu.visible):
            self._update_float_status()
            return
        children = self._get_menu_children_cached(force=force)
        refresh_all = getattr(menu, 'refresh_child_menus', None)
        if callable(refresh_all):
            refresh_all(children, force=force)
        else:
            for name, items in children.items():
                menu.refresh_child_menu(name, items)
        self._update_float_status()

    def _build_menu_children(self):
        """动态构建子菜单 (支持状态反映) — SAO Auto (6 categories)"""
        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS)

        def _k(key_id):
            v = hk.get(key_id, DEFAULT_HOTKEYS.get(key_id, ''))
            return f'  [{v}]' if v else ''

        recog_label = '识别: ON' if getattr(self, '_recognition_active', False) else '识别: OFF'
        topmost_label = '置顶: ON' if self._float.attributes('-topmost') else '置顶: OFF'

        ak_config = self._load_auto_key_config() if self._cfg_settings_ref else {}
        ak_on = bool(ak_config.get('enabled', False))
        ak_label = f'AutoKey: {"ON" if ak_on else "OFF"}' + _k('toggle_auto_script')
        hs_on = bool(self._hide_seek_engine and self._hide_seek_engine.running)
        hs_label = f'自动躲猫猫: {"ON" if hs_on else "OFF"}' + _k('toggle_hide_seek')

        br_config = self._load_boss_raid_config() if self._cfg_settings_ref else {}
        br_on = bool(br_config.get('enabled', False))
        br_label = f'BossRaid: {"ON" if br_on else "OFF"}' + _k('boss_raid_start')

        snd_on = bool(self._get_setting('sound_enabled', True))
        dps_on = bool(self._get_setting('dps_enabled', True))
        dps_report_available = self._get_dps_last_report_available()
        burst_on = bool(self._get_setting('burst_enabled', True))
        burst_slots = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not burst_slots:
            burst_slots = [1]
        burst_slot_set = set(burst_slots)
        burst_slots_disp = ','.join(str(s) for s in burst_slots)
        boss_bar_mode = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        boss_bar_labels = {'always': '常显', 'boss_raid': 'Boss战', 'off': '关闭'}
        boss_bar_disp = boss_bar_labels.get(boss_bar_mode, boss_bar_mode)

        auto_items = [
            {'icon': '⚡', 'label': ak_label, 'command': self._toggle_auto_script},
            {'icon': '◈', 'label': hs_label, 'command': self._toggle_hide_seek},
            {'icon': '◆', 'label': 'AutoKey Quick Panel', 'command': self._toggle_autokey_panel},
            {'icon': '◇', 'label': 'AutoKey Detail Editor', 'command': self._toggle_autokey_detail_panel},
        ]

        boss_items = [
            {'icon': '⚔', 'label': br_label, 'command': self._toggle_boss_raid},
            {'icon': '▸', 'label': '下一阶段' + _k('boss_raid_next_phase'), 'command': self._boss_raid_next_phase},
            {'icon': '◆', 'label': 'BossRaid Quick Panel', 'command': self._toggle_bossraid_panel},
            {'icon': '◇', 'label': 'BossRaid Detail Editor', 'command': self._toggle_bossraid_detail_panel},
        ]

        burst_items = [
            {'icon': '◆', 'label': f'爆发提示: {"ON" if burst_on else "OFF"}', 'command': self._toggle_burst_enabled},
            {'icon': '◇', 'label': f'Burst技能槽: [{burst_slots_disp}]',
             'command': lambda: self._show_entity_alert('BURST SKILLS', f'当前槽位: {burst_slots_disp}', display_time=3.0)},
            {'icon': '─', 'label': '──────────'},
        ]
        for slot in range(1, 10):
            burst_items.append({
                'icon': '◆' if slot in burst_slot_set else '◇',
                'label': f'Burst槽 {slot}' + (' ✓' if slot in burst_slot_set else ''),
                'command': lambda s=slot: self._toggle_burst_slot(s),
            })

        panel_items = [
            {'icon': '◈', 'label': 'Commander', 'command': self._toggle_commander_panel},
            {'icon': '◉', 'label': '状态面板', 'command': self._toggle_status_panel},
            {'icon': '◈', 'label': '一键隐藏面板' + (' ✓' if self._panels_hidden else ''), 'command': self._toggle_hide_all_panels},
            {'icon': '─', 'label': '──────────'},
            {'icon': '♪', 'label': f'音效: {"ON" if snd_on else "OFF"}', 'command': self._toggle_sound_enabled},
            {'icon': '♪', 'label': '音量+', 'command': lambda: self._adj_sound_volume(10)},
            {'icon': '♪', 'label': '音量-', 'command': lambda: self._adj_sound_volume(-10)},
            {'icon': '◆', 'label': f'DPS面板: {"ON" if dps_on else "OFF"}', 'command': self._toggle_dps_enabled},
            {'icon': '◆' if dps_report_available else '◇',
             'label': '查看上次战斗DPS' + (' ✓' if dps_report_available else ' (暂无)'),
             'command': self._show_last_dps_report_menu},
            {'icon': '◇', 'label': f'Boss血条: {boss_bar_disp}', 'command': self._cycle_boss_bar_mode},
        ]

        return {
            '控制': [
                {'icon': '⚙', 'label': recog_label + _k('toggle_recognition'), 'command': self._toggle_recognition_menu},
                {'icon': '⬆', 'label': topmost_label + _k('toggle_topmost'), 'command': self._toggle_topmost},
                {'icon': '✓', 'label': '保存设置', 'command': lambda: self.settings.save()},
            ],
            '自动': auto_items,
            'Boss': boss_items,
            'Burst': burst_items,
            '面板': panel_items,
            '关于': [
                {'icon': '◇', 'label': '关于本程序', 'command': self._show_about},
                {'icon': '⬇', 'label': self._build_update_menu_label(), 'command': self._check_for_updates_interactive},
                {'icon': '✎', 'label': '修改角色资料', 'command': self._edit_profile},
                {'icon': '◇', 'label': '切换到 WebView UI', 'command': self._switch_to_webview_ui},
                {'icon': '✕', 'label': '退出', 'command': self._on_close},
            ],
        }

    def _dismiss_sao_menu_for_panel(self):
        """SAO 菜单关掉再弹面板, 避免 topmost overlay 压在面板上看不见."""
        try:
            menu = getattr(self, '_sao_menu', None)
            if menu is not None and getattr(menu, 'visible', False):
                menu.close()
        except Exception:
            pass

    def _raise_panel_window(self, panel):
        """把面板提到最前并取焦, 防止被 SAO overlay 或其他 topmost 挡住."""
        if panel is None:
            return
        win = getattr(panel, '_win', None)
        if win is None:
            return
        try:
            if not win.winfo_exists():
                return
            win.attributes('-topmost', True)
            win.lift()
            win.focus_force()
        except Exception:
            pass

    def _toggle_autokey_panel(self):
        """打开/关闭 AutoKey 配置面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._autokey_panel:
            self._autokey_panel = AutoKeyPanel(
                master=self.root,
                load_fn=self._load_auto_key_config,
                save_fn=self._save_auto_key_config,
                engine_ref=lambda: self._auto_key_engine,
                on_toggle=self._toggle_auto_script,
                author_fn=getattr(self, '_auto_key_author_snapshot', None),
                load_burst_actions=self._load_autokey_burst_actions,
                save_burst_actions=self._save_autokey_burst_actions,
            )
        self._autokey_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._autokey_panel))

    def _toggle_bossraid_panel(self):
        """打开/关闭 BossRaid 配置面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
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
        self.root.after(120, lambda: self._raise_panel_window(self._bossraid_panel))

    def _toggle_autokey_detail_panel(self):
        """Open/close the full AutoKey profile editor."""
        self._dismiss_sao_menu_for_panel()
        if not self._autokey_detail_panel:
            self._autokey_detail_panel = AutoKeyDetailPanel(
                master=self.root,
                load_fn=self._load_auto_key_config,
                save_fn=self._save_auto_key_config,
                author_fn=getattr(self, '_auto_key_author_snapshot', None),
            )
        self._autokey_detail_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._autokey_detail_panel))

    def _toggle_bossraid_detail_panel(self):
        """Open/close the full BossRaid profile editor."""
        self._dismiss_sao_menu_for_panel()
        if not self._bossraid_detail_panel:
            self._bossraid_detail_panel = BossRaidDetailPanel(
                master=self.root,
                load_fn=self._load_boss_raid_config,
                save_fn=self._save_boss_raid_config,
                author_fn=getattr(self, '_boss_raid_author_snapshot', None),
            )
        self._bossraid_detail_panel.toggle()
        self.root.after(120, lambda: self._raise_panel_window(self._bossraid_detail_panel))

    def _toggle_commander_panel(self):
        """打开/关闭 Commander 面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._commander_panel:
            self._commander_panel = CommanderPanel(self.root)
        if self._commander_panel.is_visible():
            self._commander_panel.hide()
        else:
            self._commander_panel.show()
            self._push_commander_data()
            self.root.after(120, lambda: self._raise_panel_window(self._commander_panel))

    def _push_commander_data(self):
        """Build + push a snapshot to the Commander panel."""
        if not self._commander_panel or not self._commander_panel.is_visible():
            return
        try:
            bridge = getattr(self, '_packet_engine', None)
            if bridge and hasattr(bridge, 'get_commander_data'):
                data = bridge.get_commander_data()
            else:
                data = {'members': [], 'team_id': 0,
                        'leader_uid': 0, 'dungeon_id': 0}
            self._commander_panel.update(data)
        except Exception:
            pass

    def _setup_sao_menu(self):
        """构建 SAO PopUpMenu 菜单 = 主界面 (6 categories)"""
        self._menu_icons = [
            {'name': '控制', 'icon': '⚙', 'can_active': True},
            {'name': '自动', 'icon': '⚡', 'can_active': True},
            {'name': 'Boss', 'icon': '⚔', 'can_active': True},
            {'name': 'Burst', 'icon': 'B', 'can_active': True},
            {'name': '面板', 'icon': '◆', 'can_active': True},
            {'name': '关于', 'icon': 'ℹ', 'can_active': True},
        ]

        self._sao_menu = SAOPopUpMenu(
            self.root, self._menu_icons, self._get_menu_children_cached(force=True),
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
            self._stop_float_breath()
            try:
                if self._float and self._float.winfo_exists():
                    self._float.update_idletasks()
                    self._breath_base_x = self._float.winfo_x()
                    self._breath_base_y = self._float.winfo_y()
            except Exception:
                pass
            try:
                play_sound('menu_open')
            except Exception:
                pass
            self._play_motion_blur(closing=False)
            self._sao_menu.child_menus = self._get_menu_children_cached(force=True)
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
        try:
            self.root.after(90, self._restore_entity_menu_state)
        except Exception:
            pass

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
        for p in (self._status_panel, self._update_panel):
            try:
                if p and p.winfo_exists():
                    return True
            except Exception:
                pass
        for panel in (
            self._autokey_panel,
            self._bossraid_panel,
            self._autokey_detail_panel,
            self._bossraid_detail_panel,
            self._commander_panel,
        ):
            try:
                if panel and panel.is_visible():
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
        self._cancel_pending_menu_refresh()
        self._persist_entity_menu_state(save_now=False)
        self._player_panel = None
        self._maybe_stop_fisheye()
        if not self._destroyed:
            pass  # 呼吸动画已禁用 (固定位置)

    def _refresh_menu_if_open(self, force: bool = False):
        """如果菜单打开, 刷新子菜单和面板"""
        menu = getattr(self, '_sao_menu', None)
        if self._destroyed:
            return
        if not (menu and menu.visible):
            self._update_float_status()
            return
        if force:
            self._menu_refresh_force = True
        if self._menu_refresh_after_id:
            return
        try:
            self._menu_refresh_after_id = self.root.after_idle(self._apply_menu_refresh_if_open)
        except Exception:
            self._apply_menu_refresh_if_open()

    # ══════════════════════════════════════════════
    #  浮动面板: 钢琴 / 可视化
    # ══════════════════════════════════════════════
    def _toggle_status_panel(self):
        """浮动状态面板 — 显示识别状态 + 引擎信息"""
        if self._status_panel and self._status_panel.winfo_exists():
            self._fade_panel_out(self._status_panel, '_status_panel', 'show_status')
            return

        try: play_sound('panel')
        except: pass
        sw, sh = 236, 178
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

        # 更新状态行
        self._status_update_lbl = _sao_row(body_pad, '更新', '待机',
                                           value_fg='#556677',
                                           value_font=get_cjk_font(9, True))
        self._status_update_progress_lbl = _sao_row(body_pad, '进度', '--',
                                                    value_fg=_SAO_PANEL_ACCENT,
                                                    value_font=get_cjk_font(9, True))

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
        if hasattr(self, '_status_source_lbl'):
            src_text = 'Packet'
            if getattr(self, '_cfg_settings_ref', None):
                src = self._cfg_settings_ref.get('data_source', 'packet')
                src_text = 'Packet' if src == 'packet' else 'OCR'
            self._status_source_lbl.configure(text=src_text, fg=_SAO_PANEL_GOLD)
        view = self._get_update_view()
        if hasattr(self, '_status_update_lbl'):
            self._status_update_lbl.configure(text=view['status_text'], fg=view['status_color'])
        if hasattr(self, '_status_update_progress_lbl'):
            self._status_update_progress_lbl.configure(text=view['progress_text'], fg=view['progress_color'])

    def _get_update_snapshot(self):
        try:
            from sao_updater import get_manager
            return get_manager().snapshot()
        except Exception:
            return None

    def _get_update_view(self, snapshot=None):
        snap = snapshot or self._update_snapshot or self._get_update_snapshot()
        state = getattr(snap, 'state', 'idle') if snap else 'idle'
        latest_version = str(getattr(snap, 'latest_version', '') or '') if snap else ''
        package_type = str(getattr(snap, 'package_type', '') or '') if snap else ''
        notes = str(getattr(snap, 'notes', '') or '').strip() if snap else ''
        error = str(getattr(snap, 'error', '') or '').strip() if snap else ''
        skipped_version = str(getattr(snap, 'skipped_version', '') or '') if snap else ''
        force_required = bool(getattr(snap, 'force_required', False)) if snap else False
        try:
            progress = max(0.0, min(1.0, float(getattr(snap, 'progress', 0.0) or 0.0))) if snap else 0.0
        except Exception:
            progress = 0.0
        package_text = '模块增量包' if package_type == 'runtime-delta' else ('完整更新包' if package_type == 'full-package' else '更新包')
        view = {
            'state': state,
            'latest_version': latest_version,
            'force_required': force_required,
            'progress': progress,
            'show_panel': False,
            'show_progress': False,
            'status_text': '待机',
            'status_color': '#556677',
            'progress_text': '--',
            'progress_color': '#8896a8',
            'badge_text': 'IDLE',
            'badge_color': '#556677',
            'headline': '等待更新检查',
            'version_text': f'当前 {APP_VERSION_LABEL}',
            'meta_text': '菜单中的“检查更新”可主动拉取远端版本信息。',
            'notes_text': '当检测到新版本时，将自动显示下载与重启入口。',
            'primary_text': '检查更新',
            'primary_action': 'check',
            'secondary_text': '',
            'secondary_action': None,
        }
        if state == 'checking':
            view.update({
                'status_text': '检查中',
                'status_color': _SAO_PANEL_ACCENT,
                'badge_text': 'CHECKING',
                'badge_color': _SAO_PANEL_ACCENT,
                'headline': '正在检查更新',
                'meta_text': '正在连接更新服务…',
                'notes_text': '请稍候，系统正在比较本地版本与远端 manifest。',
                'primary_text': '检查中...',
                'primary_action': None,
            })
        elif state == 'available':
            skipped = bool(latest_version and skipped_version == latest_version and not force_required)
            view.update({
                'show_panel': not skipped,
                'status_text': '已跳过' if skipped else '可更新',
                'status_color': '#8896a8' if skipped else _SAO_PANEL_GOLD,
                'progress_text': latest_version and f'v{latest_version}' or '--',
                'progress_color': '#8896a8' if skipped else _SAO_PANEL_GOLD,
                'badge_text': 'SKIPPED' if skipped else ('REQUIRED' if force_required else 'AVAILABLE'),
                'badge_color': '#8896a8' if skipped else _SAO_PANEL_GOLD,
                'headline': '已跳过当前版本' if skipped else '检测到新版本',
                'version_text': latest_version and f'新版本 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': ('强制更新 · ' if force_required else '') + package_text,
                'notes_text': notes or ('当前版本低于服务器最低要求，请先完成更新。' if force_required else ('该版本已被标记为跳过，可手动重新检查或等待下一版。' if skipped else '下载完成后，重启应用将自动进入独立更新器。')),
                'primary_text': '重新检查' if skipped else '立即更新',
                'primary_action': 'check' if skipped else 'download',
                'secondary_text': '' if skipped else ('退出应用' if force_required else '跳过此版'),
                'secondary_action': None if skipped else ('quit' if force_required else 'skip'),
            })
        elif state == 'downloading':
            view.update({
                'show_panel': True,
                'show_progress': True,
                'status_text': f'下载中 {int(progress * 100)}%',
                'status_color': _SAO_PANEL_ACCENT,
                'progress_text': f'{int(progress * 100)}%',
                'progress_color': _SAO_PANEL_ACCENT,
                'badge_text': 'DOWNLOADING',
                'badge_color': _SAO_PANEL_ACCENT,
                'headline': '正在下载更新包',
                'version_text': latest_version and f'目标 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': f'正在拉取 {package_text}',
                'notes_text': notes or '下载完成后可直接重启应用，文件替换会交给独立 updater 完成。',
                'primary_text': '下载中...',
                'primary_action': None,
                'secondary_text': '',
                'secondary_action': None,
            })
        elif state == 'ready':
            view.update({
                'show_panel': True,
                'show_progress': True,
                'progress': 1.0,
                'status_text': '已就绪',
                'status_color': '#3ad86c',
                'progress_text': '100%',
                'progress_color': '#3ad86c',
                'badge_text': 'READY',
                'badge_color': '#3ad86c',
                'headline': '更新包已下载完成',
                'version_text': latest_version and f'待切换 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': '重启应用后将进入独立更新器完成替换',
                'notes_text': notes or '当前下载已完成，点击“重启应用”即可开始替换模块与启动器。',
                'primary_text': '重启应用',
                'primary_action': 'apply',
                'secondary_text': '' if force_required else '稍后',
                'secondary_action': None if force_required else 'dismiss',
            })
        elif state == 'error':
            view.update({
                'show_panel': True,
                'status_text': '错误',
                'status_color': '#ff707a',
                'progress_text': '失败',
                'progress_color': '#ff707a',
                'badge_text': 'ERROR',
                'badge_color': '#ff707a',
                'headline': '更新流程失败',
                'version_text': latest_version and f'目标 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': error or '更新服务暂不可用',
                'notes_text': error or '可以稍后重新检查一次。',
                'primary_text': '重新检查',
                'primary_action': 'check',
                'secondary_text': '关闭',
                'secondary_action': 'dismiss',
            })
        elif state == 'up_to_date':
            view.update({
                'status_text': '最新',
                'status_color': _SAO_PANEL_ACCENT,
                'progress_text': latest_version and f'v{latest_version}' or APP_VERSION_LABEL,
                'progress_color': _SAO_PANEL_ACCENT,
                'badge_text': 'LATEST',
                'badge_color': _SAO_PANEL_ACCENT,
                'headline': '当前已是最新版本',
                'version_text': latest_version and f'当前 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': '无需下载更新包',
                'notes_text': '未检测到比当前客户端更高的版本。',
            })
        return view

    def _ensure_updater_listener(self):
        if getattr(self, '_update_listener_installed', False):
            return
        try:
            from sao_updater import get_manager
            mgr = get_manager()
        except Exception:
            return

        def _listener(snapshot):
            try:
                self.root.after(0, lambda snap=snapshot: self._on_update_snapshot(snap))
            except Exception:
                pass

        self._update_listener = _listener
        mgr.add_listener(_listener)
        self._update_listener_installed = True
        try:
            self._on_update_snapshot(mgr.snapshot())
        except Exception:
            pass

    def _on_update_snapshot(self, snapshot):
        self._update_snapshot = snapshot
        view = self._get_update_view(snapshot)
        panel_key = f"{view['state']}:{view['latest_version']}"
        if panel_key != getattr(self, '_update_panel_state_key', ''):
            self._update_panel_hidden = False
        self._update_panel_state_key = panel_key

        self._update_status_panel()
        self._refresh_menu_if_open()

        if self._update_panel and self._update_panel.winfo_exists():
            if view['show_panel']:
                self._refresh_update_panel(snapshot)
            else:
                self._close_update_panel(persist_hidden=False)

        if not getattr(self, '_update_popup_ready', False):
            self._pending_update_popup_snapshot = snapshot
            return

        self._maybe_show_update_popup(snapshot)

        if self._panels_hidden:
            if not view['show_panel']:
                try:
                    if self._update_panel and self._update_panel.winfo_exists():
                        self._update_panel.destroy()
                except Exception:
                    pass
                self._update_panel = None
                try:
                    self._hidden_panels_snapshot = [name for name in self._hidden_panels_snapshot if name != 'updater']
                except Exception:
                    pass
            return

    def _mark_update_popup_ready(self):
        self._update_popup_ready = True
        pending = getattr(self, '_pending_update_popup_snapshot', None)
        if pending is None:
            return
        self._pending_update_popup_snapshot = None
        self._maybe_show_update_popup(pending)

    def _build_update_popup_payload(self, snapshot=None):
        view = self._get_update_view(snapshot)
        state = str(view.get('state') or 'idle')
        latest_version = str(view.get('latest_version') or '')
        force_required = bool(view.get('force_required', False))
        progress = int(round(float(view.get('progress') or 0.0) * 100.0))
        skipped_version = ''
        try:
            skipped_version = str(getattr(snapshot, 'skipped_version', '') or '')
        except Exception:
            skipped_version = ''
        if state == 'available':
            if latest_version and skipped_version == latest_version and not force_required:
                return None
            body = f'检测到新版本 v{latest_version or "?"}'
            if force_required:
                body += '\n此更新为强制更新，请尽快在 关于 > 检查更新 中完成下载。'
            else:
                body += '\n打开 SAO 菜单 > 关于 > 检查更新 可开始下载。'
            return {
                'key': f'available:{latest_version}:{int(force_required)}',
                'title': 'SYSTEM UPDATE',
                'message': body,
                'display_time': 6.5,
            }
        if state == 'downloading':
            body = f'正在下载更新包 v{latest_version or "?"}'
            body += f'\n当前进度 {progress}% ，完成后会提示重启应用。'
            return {
                'key': f'downloading:{latest_version}',
                'title': 'DOWNLOADING UPDATE',
                'message': body,
                'display_time': 5.0,
            }
        if state == 'ready':
            body = f'更新包 v{latest_version or "?"} 已下载完成'
            body += '\n打开 SAO 菜单 > 关于 > 检查更新 可立即重启应用。'
            return {
                'key': f'ready:{latest_version}',
                'title': 'UPDATE READY',
                'message': body,
                'display_time': 6.5,
            }
        if state == 'error':
            error = ''
            try:
                error = str(getattr(snapshot, 'error', '') or '').strip()
            except Exception:
                error = ''
            body = error or '更新服务暂不可用，请稍后重试。'
            return {
                'key': f'error:{latest_version}:{body}',
                'title': 'UPDATE ERROR',
                'message': body,
                'display_time': 5.2,
            }
        return None

    def _maybe_show_update_popup(self, snapshot=None):
        payload = self._build_update_popup_payload(snapshot)
        if not payload:
            return
        popup_key = str(payload.get('key') or '')
        if not popup_key or popup_key == getattr(self, '_last_update_popup_key', ''):
            return
        self._last_update_popup_key = popup_key
        self._show_entity_alert(
            str(payload.get('title') or 'SYSTEM UPDATE'),
            str(payload.get('message') or ''),
            display_time=float(payload.get('display_time') or 5.0),
        )

    def _start_update_download(self):
        try:
            from sao_updater import get_manager
            get_manager().download_async()
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'下载启动失败: {e}')

    def _start_update_check(self):
        try:
            from sao_updater import get_manager
            get_manager().check_async()
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'检查失败: {e}')

    def _skip_update_version(self):
        try:
            from sao_updater import get_manager
            get_manager().skip_current()
            self._close_update_panel(persist_hidden=True)
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'跳过版本失败: {e}')

    def _apply_downloaded_update(self):
        try:
            from sao_updater import has_pending_update, schedule_apply_on_exit
            if not has_pending_update():
                SAODialog.showinfo(self._float, '更新', '当前没有待应用的更新包。')
                return
            if not schedule_apply_on_exit():
                SAODialog.showinfo(self._float, '更新', '启动 updater 失败，请稍后重试。')
                return
            self._show_entity_alert('SYSTEM UPDATE', '正在切换到独立更新器', display_time=2.4)
            self._on_close()
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'应用更新失败: {e}')

    def _resolve_update_action(self, action_name):
        return {
            'download': self._start_update_download,
            'check': self._start_update_check,
            'skip': self._skip_update_version,
            'apply': self._apply_downloaded_update,
            'dismiss': lambda: self._close_update_panel(persist_hidden=True),
            'quit': self._on_close,
        }.get(action_name)

    def _set_update_button(self, button, text, action_name, side=tk.LEFT):
        if not text:
            if button.winfo_manager():
                button.pack_forget()
            button.command = None
            return
        button.set_text(text)
        button.command = self._resolve_update_action(action_name)
        if not button.winfo_manager():
            button.pack(side=side, padx=4)

    def _close_update_panel(self, persist_hidden=True):
        if persist_hidden:
            self._update_panel_hidden = True
        else:
            self._update_panel_hidden = False
        panel = self._update_panel
        self._update_panel = None
        if not (panel and panel.winfo_exists()):
            return
        try: play_sound('alert_close')
        except: pass
        t0 = time.time()
        dur = 0.22

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            t = min(1.0, (time.time() - t0) / dur)
            et = t * t
            try:
                panel.attributes('-alpha', max(0.0, 0.92 * (1.0 - et)))
            except Exception:
                pass
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

        _step()

    def _open_update_panel(self, snapshot=None):
        if self._panels_hidden:
            return
        self._update_panel_hidden = False
        if self._update_panel and self._update_panel.winfo_exists():
            try:
                self._update_panel.deiconify()
                self._update_panel.lift()
            except Exception:
                pass
            self._refresh_update_panel(snapshot)
            return

        try: play_sound('panel')
        except: pass
        sw, sh = 320, 228
        saved_ux = self.settings.get('update_x', None)
        saved_uy = self.settings.get('update_y', None)
        if saved_ux is not None:
            fx, fy = int(saved_ux), int(saved_uy)
        else:
            fx = self._float.winfo_x() + self._fw + 10
            fy = self._float.winfo_y() + 110
            screen_w = self._float.winfo_screenwidth()
            screen_h = self._float.winfo_screenheight()
            if fx + sw > screen_w - 10:
                fx = self._float.winfo_x() - sw - 10
            if fy + sh > screen_h - 10:
                fy = max(20, self._float.winfo_y() - sh + 36)

        self._update_panel = tk.Toplevel(self.root)
        self._update_panel.overrideredirect(True)
        self._update_panel.attributes('-topmost', True)
        self._update_panel.attributes('-alpha', 0.0)
        self._update_panel.geometry(f'{sw}x{sh}+{fx}+{fy}')
        self._update_panel.configure(bg=_SAO_PANEL_HEADER_BG)
        _apply_panel_style(self._update_panel)

        border = tk.Frame(self._update_panel, bg=_SAO_PANEL_BORDER, padx=1, pady=1)
        border.pack(fill=tk.BOTH, expand=True)
        inner = tk.Frame(border, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill=tk.BOTH, expand=True)

        hdr, close_lbl = _sao_panel_header(inner, '⬇', 'UPDATER', lambda: self._close_update_panel(True))
        body = _sao_panel_body(inner)
        body_pad = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        body_pad.pack(fill=tk.BOTH, expand=True, padx=10, pady=8)

        top_row = tk.Frame(body_pad, bg=_SAO_PANEL_BODY_BG)
        top_row.pack(fill=tk.X)
        self._update_badge = SAOStatusPill(top_row, text='IDLE', color='#556677', width=126, height=22)
        self._update_badge.pack(side=tk.LEFT)
        self._update_version_lbl = tk.Label(top_row, text=f'当前 {APP_VERSION_LABEL}',
                                            bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                                            font=get_cjk_font(8, True), anchor='e')
        self._update_version_lbl.pack(side=tk.RIGHT)

        self._update_headline_lbl = tk.Label(body_pad, text='等待更新检查',
                                             bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                                             anchor='w', justify=tk.LEFT,
                                             font=get_cjk_font(10, True))
        self._update_headline_lbl.pack(fill=tk.X, pady=(8, 2))
        # meta_text 包含中文 (例如 “正在拉取 ...”), 必须用 CJK 字体,
        # 否则 SAO UI 字体没有对应字形, 整行会显示成方框 tofu。
        self._update_meta_lbl = tk.Label(body_pad, text='',
                                         bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                                         anchor='w', justify=tk.LEFT,
                                         font=get_cjk_font(8))
        self._update_meta_lbl.pack(fill=tk.X)
        self._update_notes_lbl = tk.Label(body_pad, text='',
                                          bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                                          anchor='w', justify=tk.LEFT,
                                          wraplength=286,
                                          font=get_cjk_font(8))
        self._update_notes_lbl.pack(fill=tk.X, pady=(6, 8))

        self._update_progress_wrap = tk.Frame(body_pad, bg=_SAO_PANEL_BODY_BG)
        self._update_progress_wrap.pack(fill=tk.X)
        self._update_progress_bar = SAOProgressBar(self._update_progress_wrap, width=286, height=18)
        self._update_progress_bar.pack(fill=tk.X)
        self._update_progress_lbl = tk.Label(self._update_progress_wrap, text='0%',
                                             bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_ACCENT,
                                             anchor='e', justify=tk.RIGHT,
                                             font=get_sao_font(7, True))
        self._update_progress_lbl.pack(fill=tk.X, pady=(3, 0))

        self._update_buttons_frame = tk.Frame(body_pad, bg=_SAO_PANEL_BODY_BG)
        self._update_buttons_frame.pack(fill=tk.X, pady=(10, 0))
        self._update_primary_btn = SAOButton(self._update_buttons_frame, width=134, height=32)
        self._update_secondary_btn = SAOButton(self._update_buttons_frame, width=134, height=32)

        hud_cv = _sao_panel_hud_canvas(body)
        hud_cv.create_text(4, 8, text='SYS:UPDATER', anchor='w',
                           font=('Consolas', 6), fill='#d0d0d0')
        hud_cv.create_line(92, 8, sw - 10, 8, fill='#e8e8e8', width=1)

        _ud = {'x': 0, 'y': 0}
        def udstart(e): _ud['x'], _ud['y'] = e.x_root, e.y_root
        def udmove(e):
            dx, dy = e.x_root - _ud['x'], e.y_root - _ud['y']
            nx, ny = self._update_panel.winfo_x()+dx, self._update_panel.winfo_y()+dy
            self._update_panel.geometry(f'+{nx}+{ny}')
            _ud['x'], _ud['y'] = e.x_root, e.y_root
            self.settings.set('update_x', nx); self.settings.set('update_y', ny)
        _bind_panel_drag(hdr, close_lbl, udstart, udmove)

        self._fade_panel_in(self._update_panel, target=0.92)
        self._attach_sao_panel_fx(self._update_panel, hdr, inner)
        self._attach_panel_float(self._update_panel, phase=4.1)
        self._refresh_update_panel(snapshot)

    def _refresh_update_panel(self, snapshot=None):
        if not (self._update_panel and self._update_panel.winfo_exists()):
            return
        view = self._get_update_view(snapshot)
        if hasattr(self, '_update_badge'):
            self._update_badge.set_status(view['badge_text'], view['badge_color'])
        if hasattr(self, '_update_version_lbl'):
            self._update_version_lbl.configure(text=view['version_text'])
        if hasattr(self, '_update_headline_lbl'):
            self._update_headline_lbl.configure(text=view['headline'])
        if hasattr(self, '_update_meta_lbl'):
            self._update_meta_lbl.configure(text=view['meta_text'])
        if hasattr(self, '_update_notes_lbl'):
            self._update_notes_lbl.configure(text=view['notes_text'])
        if hasattr(self, '_update_progress_wrap'):
            if view['show_progress']:
                if not self._update_progress_wrap.winfo_manager():
                    self._update_progress_wrap.pack(fill=tk.X, before=self._update_buttons_frame)
                self._update_progress_bar.set_value(view['progress'])
                self._update_progress_lbl.configure(text=view['progress_text'], fg=view['progress_color'])
            elif self._update_progress_wrap.winfo_manager():
                self._update_progress_wrap.pack_forget()
        self._set_update_button(self._update_primary_btn, view['primary_text'], view['primary_action'], side=tk.LEFT)
        self._set_update_button(self._update_secondary_btn, view['secondary_text'], view['secondary_action'], side=tk.RIGHT)

    def _toggle_hide_all_panels(self):
        """一键隐藏/显示所有浮动面板 (不销毁, 只是 withdraw/deiconify)"""
        panels = [
            ('status',  self._status_panel),
            ('updater', self._update_panel),
            ('autokey_quick', getattr(self._autokey_panel, '_win', None)),
            ('bossraid_quick', getattr(self._bossraid_panel, '_win', None)),
            ('autokey_detail', getattr(self._autokey_detail_panel, '_win', None)),
            ('bossraid_detail', getattr(self._bossraid_detail_panel, '_win', None)),
            ('commander', getattr(self._commander_panel, '_win', None)),
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
        if self.settings.get('show_status', False):
            if not (self._status_panel and self._status_panel.winfo_exists()):
                self._toggle_status_panel()

    # ──────────────────────────────────────────
    #  HP overlay click / context-menu hooks
    # ──────────────────────────────────────────

    def _hp_overlay_on_click(self):
        """Left-click on HP panel: open the SAO radial menu (web parity)."""
        try:
            self._toggle_sao_menu()
        except Exception:
            pass

    def _hp_overlay_on_menu(self, x_root: int, y_root: int):
        """Right-click on HP panel: web-parity context menu."""
        try:
            menu = tk.Menu(self.root, tearoff=0,
                           bg='#cfd0c5', fg='#3c3e32',
                           activebackground='#e9ddb7',
                           activeforeground='#aa7814',
                           relief='flat', bd=1,
                           activeborderwidth=0,
                           font=get_cjk_font(9))
            menu.add_command(
                label='◆ SAO 菜单',
                command=self._hp_overlay_on_click,
            )
            recog_on = getattr(self, '_recognition_active', False)
            recog_label = '识别: ON' if recog_on else '识别: OFF'
            menu.add_command(
                label=f'◈ {recog_label}',
                command=self._toggle_recognition_menu,
            )
            menu.add_separator()
            menu.add_command(
                label='⟲ 复原位置',
                command=self._hp_overlay_restore_position,
            )
            menu.add_command(
                label='◈ 隐藏 HP 面板',
                command=self._hp_overlay_hide,
            )
            menu.add_separator()
            menu.add_command(
                label='✕ 退出',
                command=self._on_close,
            )
            try:
                menu.tk_popup(x_root, max(0, y_root - 90))
            finally:
                menu.grab_release()
        except Exception:
            pass

    def _hp_overlay_restore_position(self):
        ov = getattr(self, '_hp_overlay', None)
        if ov is not None:
            try:
                ov.restore_position()
            except Exception:
                pass

    def _hp_overlay_hide(self):
        ov = getattr(self, '_hp_overlay', None)
        if ov is not None:
            try:
                ov.hide()
                self._hp_ov_visible = False
                try:
                    self.settings.set('hp_ov_enabled', False)
                    save = getattr(self.settings, 'save', None)
                    if callable(save):
                        save()
                except Exception:
                    pass
            except Exception:
                pass

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
            self._float.deiconify()
            self._float.lift()
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
            'toggle_hide_seek': lambda: self.root.after(0, self._toggle_hide_seek),
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
                self._reset_sta_offline_state()
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

    def _show_entity_alert(self, title: str, message: str = '', display_time: float = 5.0):
        overlay = getattr(self, '_alert_overlay', None)
        if overlay is not None:
            try:
                overlay.show_alert(title, message, display_time=display_time)
                return
            except Exception:
                pass
        if message:
            print(f'[SAO Entity] {title}: {message}')
        else:
            print(f'[SAO Entity] {title}')

    def _toggle_hide_seek(self):
        """切换自动躲猫猫引擎开关."""
        if self._hide_seek_engine and self._hide_seek_engine.running:
            self._stop_hide_seek()
        else:
            self._start_hide_seek()

    def _start_hide_seek(self):
        """启动自动躲猫猫并用 AlertOverlay 保持状态提示."""
        if self._hide_seek_engine and self._hide_seek_engine.running:
            return
        try:
            from hide_seek_engine import HideSeekEngine
            from window_locator import WindowLocator

            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()

            engine = HideSeekEngine(locator=locator, on_status=self._on_hide_seek_status)
            engine.start()
            self._hide_seek_engine = engine
            self._hide_seek_alert_active = True
            self._show_entity_alert('AUTO HIDE & SEEK', '自动躲猫猫已启动', display_time=60.0)
            self._schedule_hide_seek_alert_refresh()
            self._refresh_menu_if_open()
        except Exception as exc:
            print(f'[SAO Entity] Hide&Seek start failed: {exc}')
            import traceback
            traceback.print_exc()
            self._hide_seek_engine = None
            self._hide_seek_alert_active = False
            self._show_entity_alert('AUTO HIDE & SEEK', '启动失败，请检查游戏窗口与模板资源', display_time=4.0)
            self._refresh_menu_if_open()

    def _stop_hide_seek(self, show_alert: bool = True):
        """停止自动躲猫猫并取消持久提示刷新."""
        self._hide_seek_alert_active = False
        timer = getattr(self, '_hide_seek_alert_timer', None)
        if timer:
            try:
                timer.cancel()
            except Exception:
                pass
            self._hide_seek_alert_timer = None
        engine = self._hide_seek_engine
        self._hide_seek_engine = None
        if engine is not None:
            try:
                engine.stop()
            except Exception:
                pass
        if show_alert:
            self._show_entity_alert('AUTO HIDE & SEEK', '自动躲猫猫已关闭', display_time=3.2)
        self._refresh_menu_if_open()

    def _on_hide_seek_status(self, message: str, step: int):
        """Hide & Seek 状态回调 — 当前仅用于调试日志."""
        if message:
            print(f'[HideSeek] step={step}: {message}')

    def _schedule_hide_seek_alert_refresh(self):
        timer = getattr(self, '_hide_seek_alert_timer', None)
        if timer:
            try:
                timer.cancel()
            except Exception:
                pass
        self._hide_seek_alert_timer = threading.Timer(50.0, self._refresh_hide_seek_alert)
        self._hide_seek_alert_timer.daemon = True
        self._hide_seek_alert_timer.start()

    def _refresh_hide_seek_alert(self):
        if self._destroyed or not getattr(self, '_hide_seek_alert_active', False):
            return
        engine = getattr(self, '_hide_seek_engine', None)
        if engine is None:
            self._hide_seek_alert_active = False
            self._hide_seek_alert_timer = None
            return
        if not engine.running:
            print('[SAO Entity] Hide&Seek engine thread is no longer running')
        self._show_entity_alert('AUTO HIDE & SEEK', '自动躲猫猫运行中', display_time=60.0)
        self._schedule_hide_seek_alert_refresh()

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

    def _load_autokey_burst_actions(self):
        actions = self._get_setting('autokey_burst_actions', [])
        return list(actions or [])

    def _save_autokey_burst_actions(self, actions):
        normalized = list(actions or [])
        self._set_setting('autokey_burst_actions', normalized)
        if self._auto_key_engine:
            try:
                self._auto_key_engine.set_burst_actions(normalized)
            except Exception:
                pass
            try:
                self._auto_key_engine.invalidate()
            except Exception:
                pass

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
        return load_boss_raid_config(ref, state_snapshot=self._boss_raid_author_snapshot())

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

    @staticmethod
    def _empty_dps_snapshot():
        return {
            'encounter_active': False,
            'elapsed_s': 0.0,
            'total_damage': 0,
            'total_heal': 0,
            'total_dps': 0,
            'total_hps': 0,
            'entities': [],
        }

    def _get_dps_last_report_available(self) -> bool:
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return False
        try:
            return bool(tracker.has_last_report())
        except Exception:
            return False

    def _sync_dps_report_availability(self):
        available = self._get_dps_last_report_available()
        if self._dps_overlay:
            self._dps_overlay.set_report_available(available)
        if available == self._dps_last_report_available:
            return
        self._dps_last_report_available = available
        if getattr(self, '_sao_menu', None) is not None:
            self._refresh_menu_if_open()

    def _request_dps_live_snapshot(self):
        tracker = getattr(self, '_dps_tracker', None)
        self._dps_mode = 'live'
        if tracker:
            try:
                return tracker.get_snapshot() or self._empty_dps_snapshot()
            except Exception:
                pass
        return self._empty_dps_snapshot()

    def _get_dps_last_report(self):
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return None
        try:
            return tracker.get_last_report()
        except Exception:
            return None

    def _request_dps_last_report(self):
        report = self._get_dps_last_report()
        if report:
            self._dps_mode = 'report'
            self._dps_visible = True
            self._dps_faded = False
        return report

    def _request_dps_entity_detail(self, uid):
        """Fetch detail+skill breakdown for `uid`. Returns dict or None.

        Mirrors sao_webview.py DpsAPI.get_entity_detail. Called by the
        entity DpsOverlay when the user clicks an entity row in live mode.
        """
        tracker = getattr(self, '_dps_tracker', None)
        if not tracker:
            return None
        try:
            return tracker.get_entity_detail(int(uid or 0))
        except Exception:
            return None

    def _show_dps_live_snapshot(self, snapshot=None):
        if not self._dps_overlay:
            return False
        if snapshot is None:
            snapshot = self._request_dps_live_snapshot()
        self._dps_visible = True
        self._dps_faded = False
        self._dps_mode = 'live'
        self._dps_overlay.show_live(snapshot)
        return True

    def _show_dps_last_report(self, report=None) -> bool:
        if not self._dps_overlay:
            return False
        if report is None:
            report = self._request_dps_last_report()
        if not report:
            self._sync_dps_report_availability()
            return False
        self._dps_visible = True
        self._dps_faded = False
        self._dps_mode = 'report'
        self._dps_overlay.set_report_available(True)
        return bool(self._dps_overlay.show_last_report(report))

    def _reset_dps_tracker(self):
        tracker = getattr(self, '_dps_tracker', None)
        if tracker:
            try:
                tracker.reset()
            except Exception:
                pass
        self._sync_dps_report_availability()
        self._dps_mode = 'live'
        return self._request_dps_live_snapshot()

    def _show_last_dps_report_menu(self):
        if self._show_dps_last_report():
            return
        self._show_entity_alert(
            'DPS METER',
            '暂无上一场战斗报告 / No last combat report yet.',
            display_time=3.0,
        )

    # ── Sound / Display 开关 ──

    def _toggle_sound_enabled(self):
        from sao_sound import set_sound_enabled, get_sound_enabled
        new = not get_sound_enabled()
        set_sound_enabled(new)
        self._set_setting('sound_enabled', new)
        self._refresh_menu_if_open()

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
        self._dps_enabled = new
        if not new:
            self._dps_visible = False
            self._dps_faded = False
            self._dps_mode = 'hidden'
            if self._dps_overlay:
                self._dps_overlay.hide()
        self._refresh_menu_if_open()

    def _normalize_watched_skill_slots(self, slots):
        normalized = []
        seen = set()
        for raw in list(slots or []):
            try:
                slot = int(raw)
            except Exception:
                continue
            if 1 <= slot <= 9 and slot not in seen:
                seen.add(slot)
                normalized.append(slot)
        return normalized

    def _reset_burst_tracking_state(self):
        self._last_burst_ready = False
        self._last_burst_slot = 0
        self._last_burst_slot_shown = 0

    def _toggle_burst_enabled(self):
        cur = bool(self._get_setting('burst_enabled', True))
        self._set_setting('burst_enabled', not cur)
        self._reset_burst_tracking_state()
        self._refresh_menu_if_open()

    def _toggle_burst_slot(self, slot: int):
        watched = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not watched:
            watched = [1]
        watched_set = set(watched)
        if slot in watched_set:
            if len(watched_set) == 1:
                self._show_entity_alert('BURST SKILLS', '至少保留一个技能槽', display_time=3.0)
                return
            watched_set.remove(slot)
        else:
            watched_set.add(slot)
        self._set_setting('watched_skill_slots', sorted(watched_set))
        self._reset_burst_tracking_state()
        self._refresh_menu_if_open()

    def _cycle_boss_bar_mode(self):
        modes = ['boss_raid', 'always', 'off']
        cur = self._get_setting('boss_bar_mode', 'boss_raid') or 'boss_raid'
        idx = modes.index(cur) if cur in modes else 0
        nxt = modes[(idx + 1) % len(modes)]
        self._set_setting('boss_bar_mode', nxt)
        self._refresh_menu_if_open()

    # ══════════════════════════════════════════════
    #  其他功能
    # ══════════════════════════════════════════════
    def _toggle_topmost(self):
        current = self._float.attributes('-topmost')
        new_val = not current
        self._float.attributes('-topmost', new_val)
        for panel in [self._status_panel, self._update_panel]:
            try:
                if panel and panel.winfo_exists():
                    panel.attributes('-topmost', new_val)
            except Exception:
                pass
        self._refresh_menu_if_open()

    def _switch_to_webview_ui(self):
        """切换到 WebView UI (sao_webview.py) — 热切换"""
        def _do_switch():
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
        try:
            from sao_updater import get_manager, STATE_AVAILABLE, STATE_READY
            st = get_manager().snapshot()
            extra = ''
            if st.state in (STATE_AVAILABLE, STATE_READY) and st.latest_version:
                tag = '已下载, 待重启' if st.state == STATE_READY else '可更新'
                extra = f"\n\n[{tag}] 新版本 v{st.latest_version}"
        except Exception:
            extra = ''
        self.root.after(600, lambda: SAODialog.showinfo(
            self._float, "关于",
            f"SAO Auto — 游戏辅助 UI\n{APP_VERSION_LABEL}{extra}\n\n"
            "Alt+A 打开 SAO 菜单\n"
            "右键悬浮按钮查看更多选项"))

    def _build_update_menu_label(self) -> str:
        try:
            from sao_updater import get_manager, STATE_AVAILABLE, STATE_READY, STATE_DOWNLOADING
            st = get_manager().snapshot()
            if st.state == STATE_READY:
                return f'更新就绪 v{st.latest_version} (重启应用)'
            if st.state == STATE_AVAILABLE:
                if (not st.force_required) and getattr(st, 'skipped_version', '') == st.latest_version:
                    return '检查更新'
                tag = '强制' if st.force_required else '可更新'
                return f'[{tag}] 新版本 v{st.latest_version}'
            if st.state == STATE_DOWNLOADING:
                return f'下载中 {int(st.progress * 100)}%'
        except Exception:
            pass
        return '检查更新'

    def _check_for_updates_interactive(self):
        try:
            from sao_updater import (
                get_manager,
                STATE_AVAILABLE, STATE_UP_TO_DATE, STATE_READY,
                STATE_DOWNLOADING, STATE_ERROR,
            )
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'更新模块不可用: {e}')
            return
        if self._sao_menu.visible:
            self._sao_menu.close()
        mgr = get_manager()
        st = mgr.snapshot()

        # 已就绪 -> 询问是否立即重启
        if st.state == STATE_READY:
            SAODialog.ask(self._float, '更新就绪',
                          f'v{st.latest_version} 已下载完成。\n现在重启应用更新?',
                          on_ok=self._apply_downloaded_update)
            return

        # 已知有新版本 -> 直接询问是否下载
        if st.state == STATE_AVAILABLE:
            self._prompt_update_available(st)
            return

        if st.state == STATE_DOWNLOADING:
            SAODialog.showinfo(self._float, '更新', f'正在下载 ({int(st.progress * 100)}%)...')
            return

        # 否则发起一次检查
        SAODialog.showinfo(self._float, '更新', '正在检查更新...')

        def _on_status(snapshot):
            if snapshot.state == STATE_AVAILABLE:
                try:
                    self.root.after(0, lambda: self._prompt_update_available(snapshot))
                except Exception:
                    pass
                mgr.remove_listener(_on_status)
            elif snapshot.state == STATE_UP_TO_DATE:
                try:
                    self.root.after(0, lambda: SAODialog.showinfo(
                        self._float, '更新', f'已是最新版本 ({APP_VERSION_LABEL})'))
                except Exception:
                    pass
                mgr.remove_listener(_on_status)
            elif snapshot.state == STATE_ERROR:
                try:
                    self.root.after(0, lambda: SAODialog.showinfo(
                        self._float, '更新', f'检查失败: {snapshot.error}'))
                except Exception:
                    pass
                mgr.remove_listener(_on_status)

        mgr.add_listener(_on_status)
        mgr.check_async()

    def _prompt_update_available(self, snapshot):
        from sao_updater import get_manager, STATE_READY, STATE_ERROR
        mgr = get_manager()
        notes = (snapshot.notes or '').strip()
        size_mb = (snapshot.size or 0) / 1024 / 1024
        body = f'发现新版本 v{snapshot.latest_version}'
        if size_mb > 0:
            body += f' ({size_mb:.1f} MB)'
        if notes:
            body += f'\n\n{notes[:300]}'
        if snapshot.force_required:
            body += '\n\n[强制更新] 必须升级才能继续使用'

        def _do_download():
            def _on_dl(snap):
                if snap.state == STATE_READY:
                    try:
                        self.root.after(0, lambda: SAODialog.ask(
                            self._float, '更新就绪',
                            f'v{snap.latest_version} 已下载. 现在重启应用?',
                            on_ok=self._apply_downloaded_update))
                    except Exception:
                        pass
                    mgr.remove_listener(_on_dl)
                elif snap.state == STATE_ERROR:
                    try:
                        self.root.after(0, lambda: SAODialog.showinfo(
                            self._float, '更新', f'下载失败: {snap.error}'))
                    except Exception:
                        pass
                    mgr.remove_listener(_on_dl)
            mgr.add_listener(_on_dl)
            mgr.download_async()

        if snapshot.force_required:
            # 强制: 只能更新或退出
            def _quit():
                self._on_close()
            SAODialog.ask(self._float, '强制更新—仅可选择 更新/退出', body,
                          on_ok=_do_download, on_cancel=_quit)
        else:
            SAODialog.ask(self._float, '发现更新', body,
                          on_ok=_do_download)

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
            tex = ctx.texture((width, height), 4)
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
            raw = fbo.read(components=4, alignment=1)
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(ov['sh'], ov['sw'], 4)
            photo = ImageTk.PhotoImage(Image.fromarray(arr[::-1], 'RGBA'))
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
        ignite_t = min(1.0, progress / 0.30)
        deploy_t = max(0.0, min(1.0, (progress - 0.10) / 0.70))
        settle_t = max(0.0, min(1.0, (progress - 0.80) / 0.20))
        bloom = ease_out(ignite_t)
        deploy = ease_in_out(deploy_t)
        settle = ease_in_out(settle_t)
        cx = int(lerp(ov['start_x'], ov['end_x'], deploy))
        cy = int(lerp(ov['start_y'], ov['end_y'], deploy))
        cyan = '#86dfff'
        gold = '#f3af12'
        white = '#edf7ff'
        dim_cyan = '#173746'
        dim_gold = '#5e4211'
        if progress <= 0.68:
            boot_t = progress / 0.68
        else:
            boot_t = 1.0

        try:
            if progress < 0.14:
                overlay_alpha = lerp(0.18, 0.94, ease_out(progress / 0.14))
            elif progress < 0.80:
                overlay_alpha = lerp(0.94, 0.74, ease_in_out((progress - 0.14) / 0.66))
            else:
                overlay_alpha = 0.74 * (1.0 - settle)
            win.attributes('-alpha', max(0.0, min(0.94, overlay_alpha)))
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

        if settle > 0.82:
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
        total = 1.16
        phase1 = 0.34

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
            self.root.after(220, self._mark_update_popup_ready)

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
                self._set_float_alpha(0.12 * hold)
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
    float ang = atan(p.y, p.x);
    float progress = clamp(u_progress, 0.0, 1.0);
    vec2 dirA = normalize(vec2(1.0, 0.0));
    vec2 dirB = normalize(vec2(0.5, 0.8660254));
    vec2 dirC = normalize(vec2(-0.5, 0.8660254));
    float hexScale = mix(20.0, 34.0, smoothstep(0.14, 0.86, progress));
    float lineA = gridLine(p, dirA, hexScale, 0.030);
    float lineB = gridLine(p, dirB, hexScale, 0.028);
    float lineC = gridLine(p, dirC, hexScale, 0.028);
    float ringRadius = mix(0.02, 0.92, smoothstep(0.0, 0.80, progress));
    float ringWidth = mix(0.090, 0.014, progress);
    float ring = band(r, ringRadius, ringWidth);
    float echoInner = band(r, max(0.0, ringRadius - 0.085), ringWidth * 2.2) * (1.0 - smoothstep(0.26, 0.84, progress));
    float echoOuter = band(r, ringRadius + 0.055, ringWidth * 1.5) * (1.0 - smoothstep(0.48, 0.94, progress));
    float hexMask = band(r, ringRadius - 0.018, ringWidth * 3.8);
    float grid = max(lineA, max(lineB, lineC)) * hexMask;
    float arc = smoothstep(0.18, 1.0, 0.5 + 0.5 * sin(ang * 12.0 - progress * 18.0));
    float core = exp(-r * mix(58.0, 22.0, progress));
    float halo = exp(-r * 3.4) * smoothstep(0.00, 0.18, progress) * (1.0 - smoothstep(0.62, 1.0, progress));
    float sweep = band(uv.y, 0.18 + progress * 0.60, 0.035) + band(uv.y, 0.58 - progress * 0.08, 0.055) * 0.55;
    float closeV = smoothstep(0.82, 1.0, progress);
    float closeH = mix(0.50, 0.006, closeV);
    float closeW = mix(0.50, 0.018, closeV);
    float tvMaskY = 1.0 - smoothstep(closeH, closeH + 0.02, abs(uv.y - 0.5));
    float tvMaskX = 1.0 - smoothstep(closeW, closeW + 0.02, abs(uv.x - 0.5));
    float tvMask = mix(1.0, tvMaskY, smoothstep(0.82, 0.94, progress));
    tvMask *= mix(1.0, tvMaskX, smoothstep(0.92, 1.0, progress));
    float scanlines = 0.92 + 0.08 * sin((uv.y * u_resolution.y) * 2.4 + progress * 1500.0);
    float vignette = smoothstep(1.34, 0.28, r);

    vec3 cyan = vec3(0.60, 0.95, 1.0);
    vec3 blue = vec3(0.06, 0.48, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 gold = vec3(1.0, 0.80, 0.28);
    vec3 color = vec3(0.0);

    color += mix(blue, cyan, 0.42) * ring * (0.58 + 0.42 * arc);
    color += cyan * echoInner * 0.82;
    color += gold * echoOuter * 0.44;
    color += mix(cyan, gold, 0.34) * grid * 0.58;
    color += white * core * (0.26 + 0.74 * (1.0 - smoothstep(0.16, 0.60, progress)));
    color += cyan * halo * 0.40;
    color += vec3(0.82, 0.96, 1.0) * sweep * (0.08 + ring * 0.24);
    color *= scanlines * vignette * tvMask;
    color = clamp(color, 0.0, 1.0);
    float alpha = clamp((ring * 0.88 + echoInner * 0.52 + grid * 0.30 + core * 0.82 + sweep * 0.16) * tvMask, 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 4)
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
            gl['ctx'].clear(0.0, 0.0, 0.0, 0.0)
            prog['u_center'].value = (float(cx), float(cy))
            prog['u_progress'].value = float(max(0.0, min(1.0, purge_t)))
            vao.render()
            raw = fbo.read(components=4, alignment=1)
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(ov['sh'], ov['sw'], 4)
            photo = ImageTk.PhotoImage(Image.fromarray(arr[::-1], 'RGBA'))
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
        lock_t = min(1.0, progress / 0.30)
        purge_t = max(0.0, min(1.0, (progress - 0.18) / 0.82))
        lock_e = ease_out(lock_t)
        purge_e = ease_in_out(purge_t)
        cyan = '#86dfff'
        gold = ov['banner']['accent']
        dim_cyan = '#173746'
        dim_gold = ov['banner']['accent_dim']
        white = '#edf7ff'

        wash = 0.22 + 0.78 * lock_e
        sweep = ((lock_t * 0.45) + purge_t * 1.2) % 1.0
        tv_fade = 0.0 if progress <= 0.84 else min(1.0, max(0.0, (progress - 0.84) / 0.16))
        tv_fade = ease_in_out(tv_fade)

        try:
            peak_alpha = min(0.96, 0.16 + 0.50 * lock_e + 0.18 * purge_e)
            win.attributes('-alpha', max(0.0, peak_alpha * (1.0 - tv_fade * 0.97)))
        except Exception:
            pass

        cv.delete('all')
        if purge_t < 0.06 or not ov.get('gl'):
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

        if tv_fade > 0.18:
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

        def _add_panel_owner(panel, order=0):
            if not panel:
                return
            try:
                if hasattr(panel, 'is_visible') and not panel.is_visible():
                    return
            except Exception:
                pass
            win = getattr(panel, '_win', None)
            if not win:
                return
            try:
                if hasattr(win, 'state') and str(win.state()) == 'withdrawn':
                    return
            except Exception:
                pass
            _add(win, 'panel', order=order)

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
        for idx, panel in enumerate([
            self._status_panel,
            getattr(self._autokey_panel, '_win', None),
            getattr(self._bossraid_panel, '_win', None),
            getattr(self._autokey_detail_panel, '_win', None),
            getattr(self._bossraid_detail_panel, '_win', None),
        ]):
            _add(panel, 'panel', order=idx)
        _add_panel_owner(self._commander_panel, order=5)
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
        try:
            if self._state_mgr:
                self._state_mgr.unsubscribe(self._on_game_state_update)
        except Exception:
            pass
        self._stop_fisheye_overlay()
        try:
            self._sao_menu.unbind_events()
            self._sao_menu.force_destroy_overlay()
        except Exception:
            pass
        # 停止识别引擎
        self._recognition_active = False
        self._stop_recognition_engines()
        # 保存缓存
        if self._state_mgr and self._cfg_settings_ref:
            try:
                self._persist_entity_menu_state(save_now=False)
                self._persist_cached_identity_state(save_now=False)
                self._state_mgr.save_cache(self._cfg_settings_ref)
            except Exception:
                pass
        elif self._cfg_settings_ref:
            try:
                self._persist_entity_menu_state(save_now=False)
                self._persist_cached_identity_state(save_now=True)
            except Exception:
                pass
        # 销毁所有浮动面板
        for panel in [self._status_panel, self._update_panel]:
            try:
                if panel and panel.winfo_exists():
                    panel.destroy()
            except Exception:
                pass
        # 销毁 ULW 覆盖层 + 配置面板
        for ov in [self._dps_overlay, self._boss_hp_overlay, self._hp_overlay, self._alert_overlay, self._skillfx_overlay]:
            try:
                if ov:
                    ov.destroy()
            except Exception:
                pass
        for pnl in [
            self._autokey_panel,
            self._bossraid_panel,
            self._autokey_detail_panel,
            self._bossraid_detail_panel,
            self._commander_panel,
        ]:
            try:
                if pnl:
                    pnl.destroy()
            except Exception:
                pass
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._hp_overlay = None
        self._alert_overlay = None
        self._skillfx_overlay = None
        self._autokey_panel = None
        self._bossraid_panel = None
        self._autokey_detail_panel = None
        self._bossraid_detail_panel = None
        self._commander_panel = None
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
                self._sao_menu.prepare_external_fade()
        except Exception:
            pass
        # ULW overlays (HP / BossHP / DPS) cannot be alpha-faded by
        # _collect_exit_windows() because their windows are layered. Ask
        # them to self-fade so the panels do not snap off-screen.
        for ov in (
            getattr(self, '_hp_overlay', None),
            getattr(self, '_boss_hp_overlay', None),
            getattr(self, '_dps_overlay', None),
        ):
            try:
                if ov is not None and hasattr(ov, 'hide'):
                    ov.hide()
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
        stage1 = 0.34
        stage2 = 0.82
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
                        new_alpha = item['alpha'] * (1.0 - 0.10 * hold)
                        if item.get('movable'):
                            dx = int(item['ux'] * item['travel'] * 0.06 * hold)
                            dy = int(item['uy'] * item['travel'] * 0.06 * hold)
                            if item.get('role') == 'float':
                                dy -= int(6 * hold)
                            try:
                                win.geometry(f'+{item["x"] + dx}+{item["y"] + dy}')
                            except Exception:
                                pass
                    else:
                        local = min(1.0, max(0.0, (elapsed - stage1 - item['delay']) / max(0.001, item['duration'])))
                        fade = ease_in_out(local)
                        base_alpha = item['alpha'] * 0.90
                        new_alpha = max(0.0, base_alpha * (1.0 - fade))
                        if item.get('movable'):
                            dx = int(item['ux'] * item['travel'] * (0.06 + 0.94 * fade))
                            dy = int(item['uy'] * item['travel'] * (0.06 + 0.94 * fade))
                            if item.get('role') == 'float':
                                dy -= int(14 + 18 * fade)
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
