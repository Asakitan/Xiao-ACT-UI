# -*- coding: utf-8 -*-
"""NerveGear button — visible circular entry point for the SAO menu.

The visible path is GPU-presented through ``GpuOverlayWindow``.  The
legacy UpdateLayeredWindow helper remains as a compatibility utility for
older callers, but Entity's SAO menu trigger no longer paints a Tk/ULW
button.
"""

from __future__ import annotations

import ctypes
import math
import time
from typing import Callable, Optional, Tuple

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    Image = ImageDraw = ImageFont = None  # type: ignore[assignment]

_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32


class _POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

SIZE = 72
_HALF = SIZE // 2
_MOUSE_BUTTON_LEFT = 0
_MOUSE_BUTTON_RIGHT = 1
_ACTION_PRESS = 1
_ACTION_RELEASE = 0

_DARK = {
    'bg0': (9, 16, 26),
    'bg1': (20, 38, 54),
    'border': (78, 218, 255),
    'border2': (243, 175, 18),
    'icon': (206, 245, 255),
    'glow': (52, 196, 248),
    'shadow': (2, 8, 14),
}
_LIGHT = {
    'bg0': (246, 251, 253),
    'bg1': (214, 235, 242),
    'border': (238, 183, 54),
    'border2': (69, 173, 211),
    'icon': (76, 93, 104),
    'glow': (243, 175, 18),
    'shadow': (96, 116, 126),
}


def _lerp_u8(a: int, b: int, t: float) -> int:
    return int(max(0, min(255, round(a + (b - a) * t))))


def _mix_rgb(a: Tuple[int, int, int], b: Tuple[int, int, int], t: float) -> Tuple[int, int, int]:
    return (_lerp_u8(a[0], b[0], t), _lerp_u8(a[1], b[1], t), _lerp_u8(a[2], b[2], t))


def _draw_gradient_disc(draw, box, top_rgb, bot_rgb, alpha: int, steps: int = 18) -> None:
    x1, y1, x2, y2 = box
    h = max(1.0, y2 - y1)
    for i in range(steps):
        t = i / max(1, steps - 1)
        inset = i * 0.72
        c = _mix_rgb(top_rgb, bot_rgb, t)
        a = int(alpha * (1.0 - 0.018 * i))
        draw.ellipse(
            [x1 + inset, y1 + inset, x2 - inset, y2 - inset],
            fill=(*c, max(0, min(255, a))),
        )


def render_button(
        theme: str = 'dark',
        glow_phase: float = 0.0,
        *,
        hover: bool = False,
        pressed: bool = False,
        alpha: float = 1.0) -> Optional["Image.Image"]:
    """Render a high-quality SIZE×SIZE RGBA NerveGear button image."""
    if Image is None:
        return None
    colors = _DARK if theme == 'dark' else _LIGHT
    scale = 4
    canvas = SIZE * scale
    img = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    def S(v: float) -> float:
        return v * scale

    hover_t = 1.0 if hover else 0.0
    press_t = 1.0 if pressed else 0.0
    glow_wave = 0.5 + 0.5 * math.sin(glow_phase)
    glow_alpha = int(42 + 34 * glow_wave + 28 * hover_t)
    outer_pad = S(2.0 - hover_t * 0.8)
    press_off = S(1.0 if pressed else 0.0)

    # Soft outer aura, drawn from large to small so DWM transparency keeps a
    # clean antialiased edge instead of the previous jagged Tk-looking disc.
    for i in range(6):
        inset = S(i * 1.8)
        a = int(glow_alpha * (1.0 - i / 6.8))
        draw.ellipse(
            [outer_pad + inset, outer_pad + inset,
             canvas - outer_pad - inset - 1, canvas - outer_pad - inset - 1],
            outline=(*colors['glow'], max(0, a)),
            width=max(1, int(S(1.15))),
        )

    shadow_box = [S(8), S(10), S(SIZE - 8), S(SIZE - 6)]
    draw.ellipse(shadow_box, fill=(*colors['shadow'], 54 if theme == 'dark' else 34))

    ring = [S(7) + press_off, S(6) + press_off,
            S(SIZE - 7) + press_off, S(SIZE - 8) + press_off]
    draw.ellipse(ring, fill=(*colors['border'], 218))
    draw.ellipse(
        [ring[0] + S(2.2), ring[1] + S(2.2), ring[2] - S(2.2), ring[3] - S(2.2)],
        fill=(*colors['border2'], 92 + int(50 * hover_t)),
    )
    inner = [ring[0] + S(4.6), ring[1] + S(4.6), ring[2] - S(4.6), ring[3] - S(4.6)]
    _draw_gradient_disc(
        draw, inner,
        _mix_rgb(colors['bg1'], colors['border'], 0.08 + 0.06 * hover_t),
        _mix_rgb(colors['bg0'], colors['shadow'], 0.18 + 0.08 * press_t),
        236,
    )

    # Top glass crescent + lower dim arc for a more SAO-Utils-like badge.
    draw.arc(
        [inner[0] + S(3), inner[1] + S(3), inner[2] - S(3), inner[3] - S(3)],
        205, 332, fill=(255, 255, 255, 76 + int(28 * hover_t)), width=max(1, int(S(1.4))),
    )
    draw.arc(
        [inner[0] + S(4), inner[1] + S(5), inner[2] - S(4), inner[3] - S(2)],
        28, 148, fill=(*colors['border'], 68), width=max(1, int(S(1.1))),
    )

    ic = colors['icon']
    cx, cy = S(_HALF) + press_off, S(_HALF) + press_off
    line_w = max(1, int(S(2.2)))
    # Redesigned NerveGear glyph: ring visor, capsule base, SAO scan line.
    draw.arc([cx - S(16), cy - S(18), cx + S(16), cy + S(11)],
             198, 342, fill=(*ic, 236), width=line_w)
    draw.rounded_rectangle(
        [cx - S(17), cy - S(3), cx + S(17), cy + S(9)],
        radius=int(S(4.5)), fill=(*ic, 172))
    draw.rounded_rectangle(
        [cx - S(12), cy - S(1), cx + S(12), cy + S(5)],
        radius=int(S(2.8)), fill=(*colors['bg0'], 92))
    draw.ellipse([cx - S(4.8), cy - S(13.2), cx + S(4.8), cy - S(3.6)],
                 fill=(*ic, 250))
    draw.line([(cx - S(8), cy + S(9)), (cx - S(15), cy + S(17))],
              fill=(*ic, 150), width=max(1, int(S(1.7))))
    draw.line([(cx + S(8), cy + S(9)), (cx + S(15), cy + S(17))],
              fill=(*ic, 150), width=max(1, int(S(1.7))))
    draw.line([(cx - S(20), cy + S(19)), (cx + S(20), cy + S(19))],
              fill=(*colors['border2'], 136 + int(42 * hover_t)), width=max(1, int(S(1.2))))

    try:
        img = img.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    except Exception:
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
    alpha = max(0.0, min(1.0, float(alpha)))
    if alpha < 0.999:
        r, g, b, a = img.split()
        a = a.point(lambda v: int(v * alpha))
        img = Image.merge('RGBA', (r, g, b, a))
    return img


try:
    import _sao_cy_pixels as _CY_PIXELS  # type: ignore[import-not-found]
except Exception:  # pragma: no cover - dev env without compiled helper
    _CY_PIXELS = None  # type: ignore[assignment]


def _premultiply_bgra(img: "Image.Image") -> bytes:
    rgba = img.convert('RGBA')
    if _CY_PIXELS is not None:
        try:
            return _CY_PIXELS.premultiply_bgra_bytes_floor(
                rgba.tobytes(), rgba.height, rgba.width, 1.0)
        except Exception:
            pass
    src = rgba.tobytes()
    out = bytearray(len(src))
    for i in range(0, len(src), 4):
        r, g, b, a = src[i], src[i + 1], src[i + 2], src[i + 3]
        out[i] = (b * a) // 255
        out[i + 1] = (g * a) // 255
        out[i + 2] = (r * a) // 255
        out[i + 3] = a
    return bytes(out)


class GpuNerveGearButton:
    """Interactive GPU overlay for the floating SAO menu trigger."""

    def __init__(self, root, x: int, y: int, theme: str = 'dark',
                 on_click: Optional[Callable[[], None]] = None,
                 on_right_click: Optional[Callable[[int, int], None]] = None,
                 on_move_end: Optional[Callable[[int, int], None]] = None,
                 on_hover: Optional[Callable[[bool], None]] = None):
        from gui_modules.entity_gpu_policy import require_entity_gpu
        from render import gpu_overlay_window as _gow

        require_entity_gpu('GpuNerveGearButton', _gow)
        self._root = root
        self._x = int(x)
        self._y = int(y)
        self._w = SIZE
        self._h = SIZE
        self._theme = theme or 'dark'
        self._on_click = on_click
        self._on_right_click = on_right_click
        self._on_move_end = on_move_end
        self._on_hover = on_hover
        self._hover = False
        self._pressed = False
        self._alpha = 1.0
        self._glow_phase = 0.0
        self._visible = False
        self._destroyed = False
        self._drag_start: Optional[Tuple[float, float, int, int]] = None
        self._last_sig = None
        self.last_error: Optional[str] = None
        self._last_error_print_t = 0.0

        pump = _gow.get_glfw_pump(root)
        self._presenter = _gow.BgraPresenter()
        self._win = _gow.GpuOverlayWindow(
            pump,
            w=self._w,
            h=self._h,
            x=self._x,
            y=self._y,
            render_fn=self._presenter.render,
            click_through=False,
            title='sao_nervegear_gpu',
        )
        self._win.set_input_callbacks(
            cursor_pos_fn=self._handle_cursor_pos,
            cursor_leave_fn=self._handle_cursor_leave,
            mouse_button_fn=self._handle_mouse_button,
        )
        self._stage_frame(force=True)

    def geometry(self, spec: str) -> None:
        if self._destroyed:
            return
        text = str(spec or '').strip()
        try:
            if 'x' in text and '+' in text:
                size_part, rest = text.split('+', 1)
                w_s, h_s = size_part.split('x', 1)
                self._w = max(1, int(float(w_s)))
                self._h = max(1, int(float(h_s)))
                x_s, y_s = rest.split('+', 1)
                self._x = int(float(x_s))
                self._y = int(float(y_s))
            elif text.startswith('+'):
                x_s, y_s = text[1:].split('+', 1)
                self._x = int(float(x_s))
                self._y = int(float(y_s))
        except Exception:
            return
        try:
            self._win.set_geometry(self._x, self._y, self._w, self._h)
        except Exception:
            pass
        self._stage_frame(force=True)

    def set_theme(self, theme: str) -> None:
        self._theme = theme or 'dark'
        self._stage_frame(force=True)

    def set_alpha(self, alpha: float) -> None:
        self._alpha = max(0.0, min(1.0, float(alpha)))
        self._stage_frame(force=True)

    def set_hover(self, hover: bool) -> None:
        if self._hover == bool(hover):
            return
        self._hover = bool(hover)
        self._stage_frame(force=True)

    def render(self, glow_phase: float = 0.0) -> None:
        self._glow_phase = float(glow_phase or 0.0)
        self._stage_frame()

    def deiconify(self) -> None:
        if self._destroyed:
            return
        self._visible = True
        try:
            self._win.show()
            self._stage_frame(force=True)
        except Exception as exc:
            self._record_error('show', exc)

    def withdraw(self) -> None:
        self._visible = False
        try:
            self._win.hide()
        except Exception as exc:
            self._record_error('hide', exc)

    def lift(self) -> None:
        if self._destroyed:
            return
        self.deiconify()

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        try:
            self._win.destroy()
        except Exception:
            pass
        try:
            self._presenter.release()
        except Exception:
            pass

    def winfo_exists(self) -> bool:
        return not self._destroyed

    def winfo_x(self) -> int:
        return int(self._x)

    def winfo_y(self) -> int:
        return int(self._y)

    def winfo_rootx(self) -> int:
        return int(self._x)

    def winfo_rooty(self) -> int:
        return int(self._y)

    def winfo_width(self) -> int:
        return int(self._w)

    def winfo_height(self) -> int:
        return int(self._h)

    def _cursor_screen_pos(self, local_x: float = 0.0,
                           local_y: float = 0.0) -> Tuple[float, float]:
        pt = _POINT()
        try:
            if _user32.GetCursorPos(ctypes.byref(pt)):
                return float(pt.x), float(pt.y)
        except Exception:
            pass
        return float(self._x) + float(local_x), float(self._y) + float(local_y)

    def _stage_frame(self, force: bool = False) -> None:
        if self._destroyed:
            return
        sig = (
            self._theme,
            round(float(self._glow_phase), 2),
            bool(self._hover),
            bool(self._pressed),
            round(float(self._alpha), 3),
            int(self._w),
            int(self._h),
        )
        if not force and sig == self._last_sig:
            return
        img = render_button(
            self._theme,
            self._glow_phase,
            hover=self._hover,
            pressed=self._pressed,
            alpha=self._alpha,
        )
        if img is None:
            return
        if img.size != (self._w, self._h):
            try:
                img = img.resize((self._w, self._h), Image.Resampling.LANCZOS)
            except Exception:
                img = img.resize((self._w, self._h), Image.LANCZOS)
        try:
            self._presenter.set_frame(_premultiply_bgra(img), self._w, self._h)
            self._win.request_redraw()
            self._last_sig = sig
            self.last_error = None
        except Exception as exc:
            self._record_error('stage_frame', exc)

    def _record_error(self, where: str, exc: BaseException) -> None:
        self.last_error = f'{where}: {type(exc).__name__}: {exc}'
        now = time.monotonic()
        if now - self._last_error_print_t < 2.0:
            return
        self._last_error_print_t = now
        try:
            print(f'[SAO] GPU NerveGear {self.last_error}', flush=True)
        except Exception:
            pass

    def _handle_cursor_pos(self, x: float, y: float) -> None:
        if not self._hover:
            self._hover = True
            if self._on_hover is not None:
                try:
                    self._on_hover(True)
                except Exception:
                    pass
        if self._drag_start is not None:
            sx, sy, wx, wy = self._drag_start
            gx, gy = self._cursor_screen_pos(x, y)
            dx, dy = gx - sx, gy - sy
            if abs(dx) >= 2 or abs(dy) >= 2:
                self.geometry(f'+{int(wx + dx)}+{int(wy + dy)}')
        self._stage_frame()

    def _handle_cursor_leave(self) -> None:
        self._hover = False
        if self._on_hover is not None:
            try:
                self._on_hover(False)
            except Exception:
                pass
        if self._drag_start is None:
            self._pressed = False
        self._stage_frame(force=True)

    def _handle_mouse_button(self, button: int, action: int,
                             _mods: int, x: float, y: float) -> None:
        if button == _MOUSE_BUTTON_RIGHT and action == _ACTION_PRESS:
            if self._on_right_click is not None:
                try:
                    gx, gy = self._cursor_screen_pos(x, y)
                    self._on_right_click(int(gx), int(gy))
                except Exception:
                    pass
            return
        if button != _MOUSE_BUTTON_LEFT:
            return
        if action == _ACTION_PRESS:
            self._pressed = True
            gx, gy = self._cursor_screen_pos(x, y)
            self._drag_start = (gx, gy, int(self._x), int(self._y))
            self._stage_frame(force=True)
            return
        if action != _ACTION_RELEASE:
            return
        start = self._drag_start
        self._drag_start = None
        self._pressed = False
        self._stage_frame(force=True)
        if start is None:
            return
        sx, sy, wx, wy = start
        gx, gy = self._cursor_screen_pos(x, y)
        moved = abs(gx - sx) >= 5 or abs(gy - sy) >= 5
        if moved:
            if self._on_move_end is not None:
                try:
                    self._on_move_end(int(self._x), int(self._y))
                except Exception:
                    pass
            return
        # A press/release with minimal movement is a menu-open click.
        if self._on_click is not None:
            try:
                self._on_click()
            except Exception:
                pass


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
