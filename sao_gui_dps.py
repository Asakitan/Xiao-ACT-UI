# sao_gui_dps.py — ULW DPS Overlay for SAO Entity UI

import tkinter as tk
import ctypes
import time
from PIL import Image, ImageDraw, ImageFont, ImageTk

# Win32 constants
_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008

class POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

class SIZE(ctypes.Structure):
    _fields_ = [('cx', ctypes.c_long), ('cy', ctypes.c_long)]

class BLENDFUNCTION(ctypes.Structure):
    _fields_ = [('BlendOp', ctypes.c_byte), ('BlendFlags', ctypes.c_byte),
                 ('SourceConstantAlpha', ctypes.c_byte), ('AlphaFormat', ctypes.c_byte)]

AC_SRC_OVER = 0
AC_SRC_ALPHA = 1

def _ulw_update(hwnd, img: Image.Image, x: int, y: int):
    """Update a layered window using PIL RGBA image."""
    w, h = img.size
    hdc_screen = _user32.GetDC(0)
    hdc_mem = _gdi32.CreateCompatibleDC(hdc_screen)
    
    # Create DIB section
    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', ctypes.c_uint32), ('biWidth', ctypes.c_int32),
                     ('biHeight', ctypes.c_int32), ('biPlanes', ctypes.c_uint16),
                     ('biBitCount', ctypes.c_uint16), ('biCompression', ctypes.c_uint32),
                     ('biSizeImage', ctypes.c_uint32), ('biXPelsPerMeter', ctypes.c_int32),
                     ('biYPelsPerMeter', ctypes.c_int32), ('biClrUsed', ctypes.c_uint32),
                     ('biClrImportant', ctypes.c_uint32)]
    
    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h  # top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0
    
    bits = ctypes.c_void_p()
    hbm = _gdi32.CreateDIBSection(hdc_mem, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    old_bm = _gdi32.SelectObject(hdc_mem, hbm)
    
    # Copy PIL image to DIB (BGRA format with premultiplied alpha)
    raw = img.tobytes('raw', 'BGRA')
    ctypes.memmove(bits, raw, len(raw))
    
    # Premultiply alpha
    buf = (ctypes.c_ubyte * len(raw)).from_address(bits.value)
    for i in range(0, len(raw), 4):
        a = buf[i + 3]
        if a < 255:
            buf[i] = (buf[i] * a) >> 8
            buf[i + 1] = (buf[i + 1] * a) >> 8
            buf[i + 2] = (buf[i + 2] * a) >> 8
    
    pt_dst = POINT(x, y)
    sz = SIZE(w, h)
    pt_src = POINT(0, 0)
    blend = BLENDFUNCTION(AC_SRC_OVER, 0, 255, AC_SRC_ALPHA)
    
    _user32.UpdateLayeredWindow(
        ctypes.c_void_p(hwnd), hdc_screen, ctypes.byref(pt_dst),
        ctypes.byref(sz), hdc_mem, ctypes.byref(pt_src),
        0, ctypes.byref(blend), 2  # ULW_ALPHA
    )
    
    _gdi32.SelectObject(hdc_mem, old_bm)
    _gdi32.DeleteObject(hbm)
    _gdi32.DeleteDC(hdc_mem)
    _user32.ReleaseDC(0, hdc_screen)


class DpsOverlay:
    """ULW-based DPS meter overlay with SAO styling."""
    
    WIDTH = 260
    ROW_HEIGHT = 22
    HEADER_HEIGHT = 28
    PADDING = 8
    MAX_ROWS = 8
    
    # SAO Colors
    BG_COLOR = (20, 22, 30, 180)        # dark semi-transparent
    HEADER_BG = (243, 175, 18, 220)     # gold
    BAR_BG = (40, 44, 55, 160)
    BAR_FILL_SELF = (243, 175, 18, 200) # gold for self
    BAR_FILL_OTHER = (100, 140, 200, 180)
    TEXT_WHITE = (255, 255, 255, 255)
    TEXT_DIM = (180, 190, 200, 230)
    BORDER_COLOR = (243, 175, 18, 100)
    
    def __init__(self, root: tk.Tk, settings=None):
        self.root = root
        self.settings = settings
        self._win = None
        self._hwnd = 0
        self._visible = False
        self._faded = False
        self._last_snapshot = None
        self._self_uid = 0
        self._x = 0
        self._y = 0
        
        # Position: right side of screen
        sw = _user32.GetSystemMetrics(0)
        sh = _user32.GetSystemMetrics(1)
        self._x = sw - self.WIDTH - 20
        self._y = sh // 3
        
        if settings:
            self._x = int(settings.get('dps_ov_x', self._x))
            self._y = int(settings.get('dps_ov_y', self._y))
    
    def show(self):
        if self._win is not None:
            return
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(f'1x1+{self._x}+{self._y}')
        self._win.update_idletasks()
        self._hwnd = int(self._win.wm_frame(), 16) if hasattr(self._win, 'wm_frame') else self._win.winfo_id()
        try:
            self._hwnd = ctypes.windll.user32.GetParent(self._win.winfo_id()) or self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()
        # Set layered
        ex = _user32.GetWindowLongW(ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
                               ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)
        self._visible = True
    
    def hide(self):
        if self._win:
            try: self._win.destroy()
            except Exception: pass
        self._win = None
        self._hwnd = 0
        self._visible = False
    
    def update(self, snapshot: dict):
        """Update the overlay with a new DPS snapshot."""
        if not self._visible or not self._hwnd:
            self.show()
        self._last_snapshot = snapshot
        self._render(snapshot)
    
    def _render(self, snapshot: dict):
        if not snapshot or not self._hwnd:
            return
        
        party = snapshot.get('party', [])
        if not party:
            return
        
        n_rows = min(len(party), self.MAX_ROWS)
        h = self.HEADER_HEIGHT + n_rows * self.ROW_HEIGHT + self.PADDING * 2 + 4
        w = self.WIDTH
        
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)
        
        # Background
        draw.rounded_rectangle([(0, 0), (w - 1, h - 1)], radius=6, fill=self.BG_COLOR,
                                outline=self.BORDER_COLOR)
        
        # Header
        draw.rounded_rectangle([(2, 2), (w - 3, self.HEADER_HEIGHT)], radius=4, fill=self.HEADER_BG)
        
        try:
            font_hdr = ImageFont.truetype('consola.ttf', 13)
            font_row = ImageFont.truetype('consola.ttf', 11)
        except Exception:
            font_hdr = ImageFont.load_default()
            font_row = font_hdr
        
        total_dps = snapshot.get('total_dps', 0)
        elapsed = snapshot.get('elapsed', 0)
        elapsed_str = f'{int(elapsed)}s' if elapsed < 3600 else f'{int(elapsed // 60)}m'
        draw.text((8, 6), f'DPS METER', fill=(0, 0, 0, 255), font=font_hdr)
        draw.text((w - 80, 6), f'{total_dps:,.0f}/s', fill=(0, 0, 0, 255), font=font_hdr)
        
        # Rows
        y = self.HEADER_HEIGHT + self.PADDING
        max_dmg = max((p.get('total_damage', 0) for p in party), default=1) or 1
        
        for i, p in enumerate(party[:self.MAX_ROWS]):
            row_y = y + i * self.ROW_HEIGHT
            name = p.get('name', f'Player {i+1}')
            if len(name) > 12:
                name = name[:11] + '…'
            dps = p.get('dps', 0)
            pct = p.get('pct', 0)
            total = p.get('total_damage', 0)
            
            # Bar background
            bar_x = 6
            bar_w = w - 12
            draw.rounded_rectangle([(bar_x, row_y), (bar_x + bar_w, row_y + self.ROW_HEIGHT - 3)],
                                    radius=3, fill=self.BAR_BG)
            
            # Bar fill
            fill_w = max(2, int(bar_w * (total / max_dmg)))
            is_self = (p.get('uid', 0) == self._self_uid and self._self_uid > 0)
            fill_color = self.BAR_FILL_SELF if is_self else self.BAR_FILL_OTHER
            draw.rounded_rectangle([(bar_x, row_y), (bar_x + fill_w, row_y + self.ROW_HEIGHT - 3)],
                                    radius=3, fill=fill_color)
            
            # Text
            draw.text((bar_x + 4, row_y + 2), name, fill=self.TEXT_WHITE, font=font_row)
            dps_text = f'{dps:,.0f}/s ({pct:.0f}%)'
            # Right-align DPS text
            try:
                tw = draw.textlength(dps_text, font=font_row)
            except Exception:
                tw = len(dps_text) * 7
            draw.text((bar_x + bar_w - tw - 4, row_y + 2), dps_text, fill=self.TEXT_DIM, font=font_row)
        
        try:
            _ulw_update(self._hwnd, img, self._x, self._y)
        except Exception as e:
            print(f'[DPS-OV] render error: {e}')
    
    def set_self_uid(self, uid: int):
        self._self_uid = uid
    
    def fade_out(self):
        self._faded = True
        # Render a dimmed version
        if self._last_snapshot and self._hwnd:
            # Just hide for simplicity
            try:
                _user32.ShowWindow(ctypes.c_void_p(self._hwnd), 0)  # SW_HIDE
            except Exception:
                pass
    
    def fade_in(self):
        self._faded = False
        if self._hwnd:
            try:
                _user32.ShowWindow(ctypes.c_void_p(self._hwnd), 5)  # SW_SHOW
            except Exception:
                pass
            if self._last_snapshot:
                self._render(self._last_snapshot)
    
    def destroy(self):
        self.hide()
