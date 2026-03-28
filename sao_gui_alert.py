# -*- coding: utf-8 -*-
"""
sao_gui_alert.py — ULW Alert Overlay for SAO Entity UI
Shows SAO-styled alert notifications (boss events, identity alerts, etc.)
"""

import tkinter as tk
import ctypes
import time
import threading
from PIL import Image, ImageDraw, ImageFont

from sao_gui_dps import _ulw_update

_user32 = ctypes.windll.user32
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008


class AlertOverlay:
    """ULW-based alert notification overlay with SAO styling.
    
    Shows brief alert messages that auto-dismiss after a timeout.
    Stacks multiple alerts vertically.
    """

    WIDTH = 360
    HEIGHT = 60
    MARGIN = 8
    DISPLAY_TIME = 5.0  # seconds

    # SAO Colors
    BG_COLOR = (30, 20, 10, 200)
    BORDER_COLOR = (243, 175, 18, 180)
    HEADER_BG = (243, 175, 18, 200)
    TEXT_WHITE = (255, 255, 255, 255)
    TEXT_GOLD = (243, 175, 18, 255)

    def __init__(self, root: tk.Tk, settings=None):
        self.root = root
        self.settings = settings
        self._alerts = []  # list of {win, hwnd, title, message, created_at, y}
        self._lock = threading.Lock()

        # Position: top-right
        sw = _user32.GetSystemMetrics(0)
        self._base_x = sw - self.WIDTH - 30
        self._base_y = 80

    def show_alert(self, title: str, message: str = ''):
        """Show an alert notification that auto-dismisses."""
        try:
            self.root.after(0, lambda: self._create_alert(title, message))
        except Exception:
            pass

    def _create_alert(self, title: str, message: str):
        with self._lock:
            # Calculate Y based on existing alerts
            y = self._base_y + len(self._alerts) * (self.HEIGHT + self.MARGIN)

        win = tk.Toplevel(self.root)
        win.overrideredirect(True)
        win.attributes('-topmost', True)
        win.geometry(f'1x1+{self._base_x}+{y}')
        win.update_idletasks()

        try:
            hwnd = ctypes.windll.user32.GetParent(win.winfo_id()) or win.winfo_id()
        except Exception:
            hwnd = win.winfo_id()

        ex = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE,
                               ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT)

        entry = {
            'win': win, 'hwnd': hwnd, 'title': title, 'message': message,
            'created_at': time.time(), 'y': y,
        }

        with self._lock:
            self._alerts.append(entry)

        self._render_alert(entry)

        # Schedule auto-dismiss
        self.root.after(int(self.DISPLAY_TIME * 1000), lambda: self._dismiss_alert(entry))

    def _render_alert(self, entry):
        w, h = self.WIDTH, self.HEIGHT
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)

        # Background
        draw.rounded_rectangle([(0, 0), (w - 1, h - 1)], radius=6,
                                fill=self.BG_COLOR, outline=self.BORDER_COLOR)

        # Gold accent bar on left
        draw.rectangle([(0, 4), (4, h - 4)], fill=(243, 175, 18, 220))

        try:
            font_title = ImageFont.truetype('consola.ttf', 13)
            font_msg = ImageFont.truetype('consola.ttf', 11)
        except Exception:
            font_title = ImageFont.load_default()
            font_msg = font_title

        # Title
        title = entry['title']
        if len(title) > 40:
            title = title[:39] + '…'
        draw.text((12, 8), title, fill=self.TEXT_GOLD, font=font_title)

        # Message
        msg = entry['message']
        if len(msg) > 50:
            msg = msg[:49] + '…'
        if msg:
            draw.text((12, 30), msg, fill=self.TEXT_WHITE, font=font_msg)

        try:
            _ulw_update(entry['hwnd'], img, self._base_x, entry['y'])
        except Exception as e:
            print(f'[Alert-OV] render error: {e}')

    def _dismiss_alert(self, entry):
        with self._lock:
            if entry in self._alerts:
                self._alerts.remove(entry)
        try:
            entry['win'].destroy()
        except Exception:
            pass

    def destroy(self):
        """Clean up all alert windows."""
        with self._lock:
            for entry in self._alerts:
                try:
                    entry['win'].destroy()
                except Exception:
                    pass
            self._alerts.clear()
