# -*- coding: utf-8 -*-
"""NerveGear button — visible circular entry point for the SAO menu.

Replaces the legacy invisible floating widget with a 64px round button
that renders via PIL + UpdateLayeredWindow (per-pixel alpha compositing).
Supports dark/light theme and an idle breathing glow animation.
"""

from __future__ import annotations

import ctypes
import math
import time
from typing import Optional

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    Image = ImageDraw = ImageFont = None  # type: ignore[assignment]

_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32

SIZE = 64
_HALF = SIZE // 2

_DARK = {
    'bg': (18, 24, 34),
    'border': (60, 180, 220),
    'icon': (180, 220, 240),
    'glow': (60, 180, 220),
}
_LIGHT = {
    'bg': (248, 248, 248),
    'border': (200, 170, 80),
    'icon': (100, 90, 70),
    'glow': (243, 175, 18),
}


def render_button(theme: str = 'dark', glow_phase: float = 0.0) -> Optional["Image.Image"]:
    """Render a SIZE×SIZE RGBA NerveGear button image."""
    if Image is None:
        return None
    colors = _DARK if theme == 'dark' else _LIGHT
    img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    glow_alpha = int(40 + 30 * math.sin(glow_phase))
    draw.ellipse([0, 0, SIZE - 1, SIZE - 1],
                 fill=(*colors['glow'], glow_alpha))

    draw.ellipse([4, 4, SIZE - 5, SIZE - 5],
                 fill=(*colors['bg'], 230))

    bw = 2
    draw.ellipse([4, 4, SIZE - 5, SIZE - 5],
                 outline=(*colors['border'], 200), width=bw)

    ic = colors['icon']
    cx, cy = _HALF, _HALF
    draw.ellipse([cx - 12, cy - 14, cx + 12, cy + 2],
                 outline=(*ic, 220), width=2)
    draw.rectangle([cx - 14, cy - 2, cx + 14, cy + 8],
                   fill=(*ic, 180))
    draw.ellipse([cx - 4, cy - 10, cx + 4, cy - 2],
                 fill=(*ic, 255))
    draw.line([(cx - 8, cy + 8), (cx - 14, cy + 16)],
              fill=(*ic, 160), width=2)
    draw.line([(cx + 8, cy + 8), (cx + 14, cy + 16)],
              fill=(*ic, 160), width=2)

    return img


def apply_layered_window(hwnd: int, img: "Image.Image") -> bool:
    """Blit an RGBA PIL image onto a layered window via UpdateLayeredWindow."""
    if img is None:
        return False
    try:
        w, h = img.size
        raw = img.tobytes('raw', 'BGRA')
        hdc_screen = _user32.GetDC(None)
        hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [
                ('biSize', ctypes.c_uint32),
                ('biWidth', ctypes.c_int32),
                ('biHeight', ctypes.c_int32),
                ('biPlanes', ctypes.c_uint16),
                ('biBitCount', ctypes.c_uint16),
                ('biCompression', ctypes.c_uint32),
                ('biSizeImage', ctypes.c_uint32),
                ('biXPelsPerMeter', ctypes.c_int32),
                ('biYPelsPerMeter', ctypes.c_int32),
                ('biClrUsed', ctypes.c_uint32),
                ('biClrImportant', ctypes.c_uint32),
            ]

        bmi = BITMAPINFOHEADER()
        bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.biWidth = w
        bmi.biHeight = -h
        bmi.biPlanes = 1
        bmi.biBitCount = 32
        bmi.biCompression = 0
        bmi.biSizeImage = w * h * 4

        ppv = ctypes.c_void_p()
        hbm = _gdi32.CreateDIBSection(
            hdc_mem, ctypes.byref(bmi), 0, ctypes.byref(ppv), None, 0)
        if not hbm:
            _gdi32.DeleteDC(hdc_mem)
            _user32.ReleaseDC(None, hdc_screen)
            return False
        ctypes.memmove(ppv, raw, len(raw))
        old = _gdi32.SelectObject(hdc_mem, hbm)

        class POINT(ctypes.Structure):
            _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

        class SIZE_S(ctypes.Structure):
            _fields_ = [('cx', ctypes.c_long), ('cy', ctypes.c_long)]

        class BLENDFUNCTION(ctypes.Structure):
            _fields_ = [
                ('BlendOp', ctypes.c_byte),
                ('BlendFlags', ctypes.c_byte),
                ('SourceConstantAlpha', ctypes.c_byte),
                ('AlphaFormat', ctypes.c_byte),
            ]

        pt_src = POINT(0, 0)
        size = SIZE_S(w, h)
        bf = BLENDFUNCTION(0, 0, 255, 1)

        _user32.UpdateLayeredWindow(
            ctypes.c_void_p(hwnd), hdc_screen, None,
            ctypes.byref(size), hdc_mem, ctypes.byref(pt_src),
            0, ctypes.byref(bf), 2)

        _gdi32.SelectObject(hdc_mem, old)
        _gdi32.DeleteObject(hbm)
        _gdi32.DeleteDC(hdc_mem)
        _user32.ReleaseDC(None, hdc_screen)
        return True
    except Exception:
        return False
