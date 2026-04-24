# -*- coding: utf-8 -*-
"""
sao_gui_alert.py — ULW Alert Overlay for SAO Entity UI

Shows SAO-styled alert modal (centered, light panel with entry/exit animation).
Matches web/alert.html: 392×194, title(68px) + body(106px) + footer(20px).

"""

import os
import sys
import tkinter as tk
import ctypes
import time
import threading
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur
from config import FONTS_DIR

from sao_gui_dps import _ulw_update

_user32 = ctypes.windll.user32
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008

_FONT_DIR = FONTS_DIR
_FONT_CACHE = {}


def _load_font(kind: str, size: int):
    key = (kind, size)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        return cached
    try:
        from sao_sound import load_sao_fonts as _load_sao_fonts
        _load_sao_fonts()
    except Exception:
        pass
    font = None
    candidates = []
    if kind == 'sao':
        candidates.extend([
            os.path.join(_FONT_DIR, 'SAOUI.ttf'),
            'segoeui.ttf', 'arial.ttf',
            os.path.join(_FONT_DIR, 'ZhuZiAYuanJWD.ttf'),
            'msyh.ttc', 'msyhbd.ttc', 'simhei.ttf',
        ])
    else:
        candidates.extend([
            os.path.join(_FONT_DIR, 'ZhuZiAYuanJWD.ttf'),
            'msyh.ttc', 'msyhbd.ttc', 'simhei.ttf', 'simsun.ttc',
            'segoeui.ttf', 'arial.ttf',
            os.path.join(_FONT_DIR, 'SAOUI.ttf'),
        ])
    for path in candidates:
        if os.path.isabs(path) and not os.path.exists(path):
            continue
        try:
            font = ImageFont.truetype(path, size)
            break
        except Exception:
            continue
    if font is None:
        for sysname in ('msyh.ttc', 'msyhbd.ttc', 'simhei.ttf', 'segoeui.ttf', 'arial.ttf'):
            try:
                font = ImageFont.truetype(sysname, size)
                break
            except Exception:
                continue
    if font is None:
        font = ImageFont.load_default()
    _FONT_CACHE[key] = font
    return font


def _draw_tracked(draw, xy, text, fill, font, spacing=0.0):
    x, y = float(xy[0]), float(xy[1])
    for ch in text:
        draw.text((round(x), round(y)), ch, fill=fill, font=font)
        bbox = font.getbbox(ch)
        x += (bbox[2] - bbox[0]) + spacing


def _tracked_text_width(text, font, spacing=0.0):
    w = 0.0
    for i, ch in enumerate(text):
        bbox = font.getbbox(ch)
        w += (bbox[2] - bbox[0])
        if i < len(text) - 1:
            w += spacing
    return w


class AlertOverlay:
    """ULW-based alert modal overlay matching web/alert.html design."""

    WIDTH = 392
    TITLE_H = 68
    BODY_H = 106
    FOOTER_H = 20
    HEIGHT = TITLE_H + BODY_H + FOOTER_H  # 194
    DISPLAY_TIME = 5.0
    ANIM_OPEN = 0.46
    ANIM_CLOSE = 0.40
    FPS = 60

    # Colors from web CSS
    TITLE_BG = (255, 255, 255, 209)       # rgba(255,255,255,0.82)
    BODY_BG = (234, 233, 233, 194)        # rgba(234,233,233,0.76)
    FOOTER_BG = (255, 255, 255, 204)      # rgba(255,255,255,0.80)
    TITLE_COLOR = (100, 99, 100, 255)
    BODY_COLOR = (100, 96, 96, 255)
    SHADOW_COLOR = (0, 0, 0, 77)          # box-shadow rgba(0,0,0,0.30)

    def __init__(self, root: tk.Tk, settings=None):
        self.root = root
        self.settings = settings
        self._active = None  # current alert entry or None
        self._lock = threading.Lock()
        self._anim_id = None

        sw = _user32.GetSystemMetrics(0)
        sh = _user32.GetSystemMetrics(1)
        self._center_x = (sw - self.WIDTH) // 2
        self._center_y = (sh - self.HEIGHT) // 2

        # Theme: load saved preference
        self._theme_name: str = 'light'
        if settings is not None:
            try:
                saved = settings.get('panel_themes', {}).get('alert', 'light')
                if saved in ('light', 'dark'):
                    self._apply_theme(saved)
            except Exception:
                pass

    def show_alert(self, title: str, message: str = '', display_time: float | None = None):
        try:
            self.root.after(0, lambda: self._create_alert(title, message, display_time))
        except Exception:
            pass

    def _create_alert(self, title: str, message: str, display_time: float | None = None):
        with self._lock:
            prev = self._active
            self._active = None
        if prev is not None:
            # 先标记为 destroyed 以让遗留的 anim/hold 回调提前退出，
            # 再销毁窗口。避免两个 alert 重叠在一起。
            prev['destroyed'] = True
            try:
                prev['win'].destroy()
            except Exception:
                pass
        try:
            if self._anim_id is not None:
                self.root.after_cancel(self._anim_id)
        except Exception:
            pass
        self._anim_id = None

        # Pre-render the static frame at full opacity
        base_img = self._render_frame(title, message)

        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.attributes('-topmost', True)
        win.geometry(f'1x1+{self._center_x}+{self._center_y}')
        win.update_idletasks()

        try:
            hwnd = ctypes.windll.user32.GetParent(win.winfo_id()) or win.winfo_id()
        except Exception:
            hwnd = win.winfo_id()

        ex = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE,
                               ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT)
        # 防御性清理：移除可能被 _apply_panel_style() 设置的 CS_DROPSHADOW
        try:
            _GCL_STYLE, _CS_DS = -26, 0x00020000
            _cls = ctypes.windll.user32.GetClassLongW(hwnd, _GCL_STYLE)
            if _cls & _CS_DS:
                ctypes.windll.user32.SetClassLongW(hwnd, _GCL_STYLE, _cls & ~_CS_DS)
        except Exception:
            pass
        try:
            _user32.SetWindowDisplayAffinity(ctypes.c_void_p(hwnd), 0x00000011)
        except Exception:
            pass

        entry = {
            'win': win, 'hwnd': hwnd, 'base_img': base_img,
            'created_at': time.time(), 'phase': 'open',
            'display_time': max(0.5, float(display_time if display_time is not None else self.DISPLAY_TIME)),
        }

        with self._lock:
            self._active = entry

        self._animate_open(entry)

    # ── Theme ──

    def _apply_theme(self, theme_name: str) -> None:
        """切换 Alert 面板主题。"""
        from sao_theme import get_panel_theme
        theme = get_panel_theme('alert', theme_name)
        if not theme:
            return
        for key, value in theme.items():
            setattr(self, key, value)
        self._theme_name = theme_name

    def _render_frame(self, title: str, message: str):
        W, H = self.WIDTH, self.HEIGHT
        # Render at 2x for shadow room, then crop
        PAD = 40  # shadow padding
        cw, ch = W + PAD * 2, H + PAD * 2
        img = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        ox, oy = PAD, PAD

        # Box shadow: 0 0 32px 4px rgba(0,0,0,0.30)
        shadow_rect = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow_rect)
        sd.rounded_rectangle(
            [(ox - 4, oy - 4), (ox + W + 4, oy + H + 4)],
            radius=6, fill=(0, 0, 0, 77)
        )
        shadow_rect = _gpu_blur(shadow_rect, 16)
        img = Image.alpha_composite(img, shadow_rect)
        draw = ImageDraw.Draw(img)

        # Title section
        draw.rounded_rectangle(
            [(ox, oy), (ox + W - 1, oy + self.TITLE_H - 1)],
            radius=2, fill=self.TITLE_BG
        )

        # Title shadow: rgba(0,0,0,.3) 0 13px 12px 0
        title_shadow = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        tsd = ImageDraw.Draw(title_shadow)
        tsd.rectangle(
            [(ox, oy + self.TITLE_H - 1), (ox + W, oy + self.TITLE_H + 12)],
            fill=(0, 0, 0, 50)
        )
        title_shadow = _gpu_blur(title_shadow, 6)
        img = Image.alpha_composite(img, title_shadow)
        draw = ImageDraw.Draw(img)

        # Body section
        draw.rectangle(
            [(ox, oy + self.TITLE_H), (ox + W - 1, oy + self.TITLE_H + self.BODY_H - 1)],
            fill=self.BODY_BG
        )

        # Footer section
        draw.rectangle(
            [(ox, oy + self.TITLE_H + self.BODY_H),
             (ox + W - 1, oy + H - 1)],
            fill=self.FOOTER_BG
        )

        # Footer gradient line at y=9 from footer top
        fy = oy + self.TITLE_H + self.BODY_H + 9
        line_left = ox + 18
        line_right = ox + W - 18
        line_w = line_right - line_left
        if line_w > 0:
            for i in range(line_w):
                t = i / line_w
                if t < 0.33:
                    # transparent → cyan
                    a = int(235 * (t / 0.33))
                    c = (104, 228, 255, a)
                elif t < 0.66:
                    # cyan → gold
                    u = (t - 0.33) / 0.33
                    r = int(104 + (243 - 104) * u)
                    g = int(228 + (175 - 228) * u)
                    b = int(255 + (18 - 255) * u)
                    a = int(235 + (209 - 235) * u)
                    c = (r, g, b, a)
                else:
                    # gold → transparent
                    a = int(209 * (1.0 - (t - 0.66) / 0.34))
                    c = (243, 175, 18, a)
                draw.line([(line_left + i, fy), (line_left + i, fy)], fill=c)

        # Footer top shadow: rgba(0,0,0,.07) 0 -10px 12px 0
        footer_shadow = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        fsd = ImageDraw.Draw(footer_shadow)
        fsy = oy + self.TITLE_H + self.BODY_H
        fsd.rectangle(
            [(ox, fsy - 10), (ox + W, fsy)],
            fill=(0, 0, 0, 18)
        )
        footer_shadow = _gpu_blur(footer_shadow, 6)
        img = Image.alpha_composite(img, footer_shadow)
        draw = ImageDraw.Draw(img)

        # Title text: font-weight 800, 18px, letter-spacing 1px
        font_title = _load_font('cjk', 18)
        if len(title) > 30:
            title = title[:29] + '…'
        tw = _tracked_text_width(title, font_title, 1.0)
        tx = ox + (W - tw) / 2
        ty = oy + (self.TITLE_H - 18) / 2
        _draw_tracked(draw, (tx, ty), title, fill=self.TITLE_COLOR, font=font_title, spacing=1.0)

        # Body text: 14px, centered, pre-line
        font_body = _load_font('cjk', 14)
        if message:
            lines = message.split('\n')
            line_h = 14 * 1.7  # line-height: 1.7
            total_h = len(lines) * line_h
            by_start = oy + self.TITLE_H + (self.BODY_H - total_h) / 2
            for i, line in enumerate(lines):
                if len(line) > 45:
                    line = line[:44] + '…'
                lw = _tracked_text_width(line, font_body, 0.0)
                lx = ox + (W - lw) / 2
                ly = by_start + i * line_h
                draw.text((round(lx), round(ly)), line, fill=self.BODY_COLOR, font=font_body)

        # Crop to final size with shadow padding preserved
        return img

    def _scale_frame(self, entry, scale, opacity):
        base = entry['base_img']
        bw, bh = base.size
        nw = max(1, int(bw * scale))
        nh = max(1, int(bh * scale))
        scaled = base.resize((nw, nh), Image.LANCZOS)

        # Apply opacity
        if opacity < 1.0:
            arr = np.array(scaled)
            arr[:, :, 3] = (arr[:, :, 3] * opacity).astype(np.uint8)
            scaled = Image.fromarray(arr)

        # Center on screen
        PAD = 40
        cx = self._center_x - PAD + (bw - nw) // 2
        cy = self._center_y - PAD + (bh - nh) // 2

        try:
            _ulw_update(entry['hwnd'], scaled, cx, cy)
        except Exception:
            pass

    def _animate_open(self, entry):
        t0 = time.time()
        dur = self.ANIM_OPEN

        def step():
            if entry.get('destroyed'):
                return
            t = min((time.time() - t0) / dur, 1.0)
            if t < 0.55:
                # 0→55%: scale 0.78→1.02, opacity 0→1
                u = t / 0.55
                scale = 0.78 + (1.02 - 0.78) * u
                opacity = u
            else:
                # 55%→100%: scale 1.02→1.0, opacity 1
                u = (t - 0.55) / 0.45
                scale = 1.02 + (1.0 - 1.02) * u
                opacity = 1.0

            self._scale_frame(entry, scale, opacity)

            if t < 1.0:
                self._anim_id = self.root.after(1000 // self.FPS, step)
            else:
                entry['phase'] = 'hold'
                hold_s = float(entry.get('display_time', self.DISPLAY_TIME))
                self.root.after(int(hold_s * 1000), lambda: self._animate_close(entry))

        step()

    def _animate_close(self, entry):
        if entry.get('destroyed'):
            return
        t0 = time.time()
        dur = self.ANIM_CLOSE
        entry['phase'] = 'close'

        def step():
            if entry.get('destroyed'):
                return
            t = min((time.time() - t0) / dur, 1.0)
            scale = 1.0 + (0.90 - 1.0) * t
            opacity = 1.0 - t

            self._scale_frame(entry, scale, opacity)

            if t < 1.0:
                self._anim_id = self.root.after(1000 // self.FPS, step)
            else:
                self._dismiss(entry)

        step()

    def _dismiss(self, entry):
        entry['destroyed'] = True
        with self._lock:
            if self._active is entry:
                self._active = None
        try:
            entry['win'].destroy()
        except Exception:
            pass

    def destroy(self):
        with self._lock:
            if self._active:
                self._active['destroyed'] = True
                try:
                    self._active['win'].destroy()
                except Exception:
                    pass
                self._active = None


# ────────────────────────────────────────────────────────────
# Theme dictionaries & registration
# ────────────────────────────────────────────────────────────

ALERT_THEME_LIGHT = {
    'TITLE_BG':     (255, 255, 255, 209),
    'BODY_BG':      (234, 233, 233, 194),
    'FOOTER_BG':    (255, 255, 255, 204),
    'TITLE_COLOR':  (100, 99, 100, 255),
    'BODY_COLOR':   (100, 96, 96, 255),
    'SHADOW_COLOR': (0, 0, 0, 77),
}

ALERT_THEME_DARK = {
    'TITLE_BG':     (18, 24, 34, 235),
    'BODY_BG':      (14, 18, 28, 220),
    'FOOTER_BG':    (20, 26, 36, 230),
    'TITLE_COLOR':  (200, 215, 230, 255),
    'BODY_COLOR':   (180, 195, 210, 255),
    'SHADOW_COLOR': (0, 0, 0, 100),
}

from sao_theme import register_panel_theme
register_panel_theme('alert', 'light', ALERT_THEME_LIGHT)
register_panel_theme('alert', 'dark', ALERT_THEME_DARK)
