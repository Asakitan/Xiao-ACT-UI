# -*- coding: utf-8 -*-
"""NerveGear button — visible circular GPU entry point for the SAO menu."""

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
    """Render a SIZE×SIZE RGBA NerveGear button — SAO-style clean disc."""
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

    cx, cy = canvas / 2, canvas / 2
    press_off = S(0.8) * press_t

    # Outer glow ring (pulsing)
    glow_a = int(30 + 22 * glow_wave + 36 * hover_t)
    for i in range(5):
        r_off = S(i * 1.4)
        a = int(glow_a * (1.0 - i / 5.5))
        draw.ellipse(
            [S(2) + r_off, S(2) + r_off,
             canvas - S(2) - r_off, canvas - S(2) - r_off],
            outline=(*colors['glow'], max(0, a)),
            width=max(1, int(S(0.9))),
        )

    # Drop shadow
    draw.ellipse(
        [S(9), S(11) + press_off, S(SIZE - 9), S(SIZE - 5) + press_off],
        fill=(*colors['shadow'], 50 if theme == 'dark' else 28),
    )

    # Main disc — thin cyan border + dark interior
    disc = [S(8) + press_off, S(7) + press_off,
            S(SIZE - 8) + press_off, S(SIZE - 9) + press_off]
    draw.ellipse(disc, fill=(*colors['border'], 200 + int(30 * hover_t)))
    inner = [disc[0] + S(2), disc[1] + S(2), disc[2] - S(2), disc[3] - S(2)]
    _draw_gradient_disc(
        draw, inner,
        _mix_rgb(colors['bg1'], colors['border'], 0.05 + 0.08 * hover_t),
        _mix_rgb(colors['bg0'], colors['shadow'], 0.12),
        240,
    )

    # Glass highlight arc (top crescent)
    draw.arc(
        [inner[0] + S(4), inner[1] + S(3), inner[2] - S(4), inner[3] - S(6)],
        210, 330, fill=(255, 255, 255, 62 + int(30 * hover_t)),
        width=max(1, int(S(1.2))),
    )

    # SAO diamond glyph — clean geometric icon
    ic = colors['icon']
    gcx = cx + press_off
    gcy = cy + press_off - S(1)

    # Outer diamond (rotated square)
    d_sz = S(13)
    pts_outer = [
        (gcx, gcy - d_sz),       # top
        (gcx + d_sz, gcy),       # right
        (gcx, gcy + d_sz),       # bottom
        (gcx - d_sz, gcy),       # left
    ]
    draw.polygon(pts_outer, fill=(*ic, 0), outline=(*ic, 220),
                 width=max(1, int(S(1.8))))

    # Inner diamond (smaller, filled)
    d_in = S(7.5)
    pts_inner = [
        (gcx, gcy - d_in),
        (gcx + d_in, gcy),
        (gcx, gcy + d_in),
        (gcx - d_in, gcy),
    ]
    draw.polygon(pts_inner, fill=(*colors['border'], 140 + int(60 * hover_t)))

    # Center dot
    dot_r = S(2.8)
    draw.ellipse(
        [gcx - dot_r, gcy - dot_r, gcx + dot_r, gcy + dot_r],
        fill=(*ic, 250),
    )

    # Bottom accent line
    line_y = disc[3] - S(4)
    line_hw = S(14)
    draw.line(
        [(gcx - line_hw, line_y), (gcx + line_hw, line_y)],
        fill=(*colors['border2'], 120 + int(50 * hover_t)),
        width=max(1, int(S(1.0))),
    )

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
        # Persistent interactive layer: route its input through a Tk
        # proxy so the shared overlay host stays click-through
        # everywhere. Otherwise this button alone forces the host out of
        # passthrough and every other layer's click region starts gating
        # input — a running desktop pet then shows a flickering
        # click/cursor dead-zone halo and clicks over it stop reaching
        # the game. No-op on the legacy GLFW path (own window).
        try:
            self._win.enable_input_proxy()
        except Exception:
            pass
        # Don't show or stage frames yet — LinkStart animation controls
        # visibility via set_alpha / deiconify at the right moment.
        self._stage_frame(force=True)
        self._win.hide()

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
        self.raise_topmost()

    def raise_topmost(self) -> None:
        if self._destroyed:
            return
        hwnd = getattr(self._win, '_hwnd', 0)
        if not hwnd:
            return
        try:
            from render.gpu_overlay_window import (
                _show_no_activate, HWND_TOPMOST, SWP_NOMOVE, SWP_NOSIZE,
                SWP_NOACTIVATE, _user32,
            )
            _user32.SetWindowPos(
                hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

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
            bgra = _premultiply_bgra(img)
            self._presenter.set_frame(bgra, self._w, self._h)
            # Unified compositor: also hand the frame to the layer
            # itself. This layer renders via render_fn (the presenter),
            # so these bytes are never drawn — they exist purely as the
            # alpha-silhouette source for the input proxy's per-pixel
            # hit shape (clicks on the transparent corners of the 72px
            # square must fall through to the game; only the disc is
            # clickable). Same pattern as Part B layers keeping an MMF
            # attached only for its alpha byte.
            delegate = getattr(self._win, '_delegate', None)
            if delegate is not None:
                try:
                    delegate.layer.upload_bgra(bgra, self._w, self._h)
                except Exception:
                    pass
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
                return
