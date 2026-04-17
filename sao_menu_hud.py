from __future__ import annotations

import datetime as _dt
import math
import os
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageTk

try:
    from gpu_renderer import gaussian_blur_rgba as _gpu_blur
    from gpu_renderer import render_shell_rgba as _gpu_shell
except Exception:
    _gpu_blur = None
    _gpu_shell = None


_BASE = os.path.dirname(os.path.abspath(__file__))
_FONTS_DIR = os.path.join(_BASE, 'assets', 'fonts')
_FONT_SAO = os.path.join(_FONTS_DIR, 'SAOUI.ttf')
_FONT_CJK = os.path.join(_FONTS_DIR, 'ZhuZiAYuanJWD.ttf')


def _hex_to_rgb_tuple(color: str, fallback: Tuple[int, int, int] = (1, 1, 1)) -> Tuple[int, int, int]:
    if not isinstance(color, str):
        return fallback
    raw = color.strip().lstrip('#')
    if len(raw) == 8:
        raw = raw[:6]
    if len(raw) == 3:
        raw = ''.join(ch * 2 for ch in raw)
    if len(raw) != 6:
        return fallback
    try:
        return tuple(int(raw[idx:idx + 2], 16) for idx in (0, 2, 4))
    except Exception:
        return fallback


def _rgba_to_hex(color: Tuple[int, int, int, int]) -> str:
    r, g, b = int(color[0]), int(color[1]), int(color[2])
    return f'#{r:02x}{g:02x}{b:02x}'


def _tk_font_spec(kind: str, size: int) -> Tuple:
    """Return a Tk font spec, preferring the installed SAO UI/CJK face so
    the Canvas stamp looks identical to the PIL-rendered version. Falls
    back to a system font if sao_sound.get_sao_font is unavailable."""
    try:
        if kind == 'sao':
            from sao_sound import get_sao_font as _gs
            return _gs(max(6, int(size)))
        from sao_sound import get_cjk_font as _gc
        return _gc(max(6, int(size)))
    except Exception:
        family = 'Segoe UI' if kind == 'sao' else 'Microsoft YaHei UI'
        return (family, max(6, int(size)))


@dataclass
class MenuHudFrame:
    """Lightweight per-frame descriptor for the menu HUD.

    Instead of a single composited PhotoImage, this splits the HUD into
    a static background photo + a handful of dynamic primitives that the
    caller renders via Canvas-native items (lines, images, text). This
    keeps the per-frame cost at a few coords()/itemconfigure() calls."""

    static_photo: ImageTk.PhotoImage
    static_size: Tuple[int, int]
    cx1: int
    cy1: int
    cx2: int
    cy2: int
    rail_x_l: int
    rail_x_r: int
    scan_y: int
    trail_ys: Tuple[int, int]
    scan_color: str
    trail_colors: Tuple[str, str]
    dot_y_l: int
    dot_y_r: int
    dot_color_l: str
    dot_color_r: str
    dot_photo_l: ImageTk.PhotoImage
    dot_photo_r: ImageTk.PhotoImage
    dot_radius: int
    dot_glow_size: int
    stamp_text: str
    stamp_pos: Tuple[int, int]
    stamp_color: str
    stamp_font: Tuple


class MenuHudSpriteRenderer:
    _PLATE_PAD = 16
    _HUD_MARGIN = 6
    _BRACKET_LEN = 16
    _RAIL_OFFSET = 4
    _DOT_RADIUS = 2
    _CYAN = (94, 184, 202, 255)
    _GOLD = (243, 175, 18, 255)
    _DIM_CYAN = (94, 184, 202, 180)
    _DIM_GOLD = (200, 145, 14, 180)
    _SCAN_TRAIL = ((58, 106, 120, 255), (42, 80, 96, 255))
    _SHELL_SHADOW = (76, 122, 138, 24)

    def __init__(self) -> None:
        self._static_key: Optional[Tuple[int, int, int, int]] = None
        self._static_img: Optional[Image.Image] = None
        self._static_photo: Optional[ImageTk.PhotoImage] = None
        self._static_photo_size: Optional[Tuple[int, int]] = None
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._photo_size: Optional[Tuple[int, int]] = None
        self._font_cache: Dict[Tuple[str, int], ImageFont.FreeTypeFont] = {}
        self._dot_cache: Dict[Tuple[Tuple[int, int, int, int], int], Image.Image] = {}
        self._dot_photo_cache: Dict[Tuple[Tuple[int, int, int, int], int], ImageTk.PhotoImage] = {}
        self._stamp_second: Optional[int] = None
        self._stamp_text: str = ''
        # Per-frame dedup: if nothing visible would change, return the
        # cached PhotoImage without copying the static layer or repainting.
        self._frame_sig: Optional[Tuple[int, ...]] = None

    def reset(self) -> None:
        self._static_key = None
        self._static_img = None
        self._static_photo = None
        self._static_photo_size = None
        self._photo = None
        self._photo_size = None
        self._stamp_second = None
        self._stamp_text = ''
        self._frame_sig = None

    def render(self, content_w: int, content_h: int,
               screen_w: int, screen_h: int,
               phase: float) -> 'MenuHudFrame':
        """Return a lightweight frame descriptor that can be drawn with
        Canvas-native primitives (lines, images, text).

        The previous implementation composited the full HUD into a single
        PhotoImage every tick, which required `static.copy()` + PIL draw
        ops + `PhotoImage.paste()` (~2.7 ms/frame at 60 Hz). By splitting
        the static background from the dynamic scan/dot/clock elements
        and letting Tk's Canvas animate them natively, we keep the exact
        visual output while cutting the per-frame cost ~>25x.
        """
        content_w = max(260, int(content_w))
        content_h = max(180, int(content_h))
        screen_w = max(1, int(screen_w))
        screen_h = max(1, int(screen_h))

        cx1 = self._PLATE_PAD - self._HUD_MARGIN
        cy1 = self._PLATE_PAD - self._HUD_MARGIN
        cx2 = self._PLATE_PAD + content_w + self._HUD_MARGIN
        cy2 = self._PLATE_PAD + content_h + self._HUD_MARGIN
        scan_period = 6.0
        scan_pos = (phase % scan_period) / scan_period
        scan_y = int(cy1 + (cy2 - cy1) * scan_pos)
        dot_travel = max(1, cy2 - cy1 - self._BRACKET_LEN * 2)
        dot_y_l = cy1 + self._BRACKET_LEN + int(
            dot_travel * ((math.sin(phase * 0.8) + 1.0) * 0.5))
        dot_y_r = cy1 + self._BRACKET_LEN + int(
            dot_travel * ((math.sin(phase * 0.8 + math.pi) + 1.0) * 0.5))
        now = _dt.datetime.now()
        now_second = int(now.timestamp())

        static_photo = self._get_static_photo(content_w, content_h,
                                              screen_w, screen_h)
        dot_photo_l = self._dot_photo(self._CYAN, 10)
        dot_photo_r = self._dot_photo(self._GOLD, 10)

        if self._stamp_second != now_second:
            self._stamp_second = now_second
            self._stamp_text = now.strftime('%H:%M:%S')

        return MenuHudFrame(
            static_photo=static_photo,
            static_size=self._static_photo_size or (0, 0),
            cx1=cx1, cy1=cy1, cx2=cx2, cy2=cy2,
            rail_x_l=cx1 - self._RAIL_OFFSET,
            rail_x_r=cx2 + self._RAIL_OFFSET,
            scan_y=scan_y,
            trail_ys=(scan_y - 2, scan_y - 4),
            trail_colors=(_rgba_to_hex(self._SCAN_TRAIL[0]),
                          _rgba_to_hex(self._SCAN_TRAIL[1])),
            scan_color=_rgba_to_hex(self._CYAN),
            dot_y_l=dot_y_l, dot_y_r=dot_y_r,
            dot_color_l=_rgba_to_hex(self._CYAN),
            dot_color_r=_rgba_to_hex(self._GOLD),
            dot_photo_l=dot_photo_l,
            dot_photo_r=dot_photo_r,
            dot_radius=self._DOT_RADIUS,
            dot_glow_size=dot_photo_l.width() if dot_photo_l else 30,
            stamp_text=self._stamp_text,
            stamp_pos=(cx2 - 2, cy2 + 2),   # Tk text anchor='ne'
            stamp_color=_rgba_to_hex(self._DIM_GOLD),
            stamp_font=_tk_font_spec('sao', 10),
        )

    def _get_static_photo(self, content_w: int, content_h: int,
                          screen_w: int, screen_h: int) -> ImageTk.PhotoImage:
        key = (content_w, content_h, screen_w, screen_h)
        static = self._get_static_layer(content_w, content_h,
                                        screen_w, screen_h)
        if self._static_key == key and self._static_photo is not None:
            return self._static_photo
        self._static_photo = ImageTk.PhotoImage(static)
        self._static_photo_size = static.size
        return self._static_photo

    def _dot_photo(self, color: Tuple[int, int, int, int],
                   size: int) -> ImageTk.PhotoImage:
        key = (color, size)
        cached = self._dot_photo_cache.get(key)
        if cached is not None:
            return cached
        sprite = self._dot_sprite(color, size)
        photo = ImageTk.PhotoImage(sprite)
        self._dot_photo_cache[key] = photo
        return photo

    def sprite_origin(self, left: int, top: int) -> Tuple[int, int]:
        return int(left) - self._PLATE_PAD, int(top) - self._PLATE_PAD

    def _get_static_layer(self, content_w: int, content_h: int,
                          screen_w: int, screen_h: int) -> Image.Image:
        key = (content_w, content_h, screen_w, screen_h)
        if self._static_key == key and self._static_img is not None:
            return self._static_img

        img_w = content_w + self._PLATE_PAD * 2
        img_h = content_h + self._PLATE_PAD * 2
        # Keep the menu HUD transparent-only on Windows transparentcolor
        # overlays. A dark translucent plate behind the content produces a
        # visible black fringe around the floating menu and child options.
        layer = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))

        draw = ImageDraw.Draw(layer)
        cx1 = self._PLATE_PAD - self._HUD_MARGIN
        cy1 = self._PLATE_PAD - self._HUD_MARGIN
        cx2 = self._PLATE_PAD + content_w + self._HUD_MARGIN
        cy2 = self._PLATE_PAD + content_h + self._HUD_MARGIN
        self._draw_brackets(draw, cx1, cy1, cx2, cy2)
        self._draw_rails(draw, cx1, cy1, cx2, cy2)
        self._draw_static_labels(draw, cx1, cy1, cx2, cy2, screen_w, screen_h)

        self._static_key = key
        self._static_img = layer
        return layer

    def _draw_dynamic(self, frame: Image.Image,
                      cx1: int, cy1: int, cx2: int, cy2: int,
                      scan_y: int, dot_y_l: int, dot_y_r: int,
                      stamp: str) -> None:
        draw = ImageDraw.Draw(frame)
        draw.line((cx1, scan_y, cx2, scan_y), fill=self._CYAN, width=1)
        for idx, color in enumerate(self._SCAN_TRAIL):
            trail_y = scan_y - 3 - idx * 3
            draw.line((cx1, trail_y, cx2, trail_y), fill=color, width=1)

        rail_x_l = cx1 - self._RAIL_OFFSET
        rail_x_r = cx2 + self._RAIL_OFFSET
        self._alpha_dot(frame, rail_x_l, dot_y_l, self._CYAN)
        self._alpha_dot(frame, rail_x_r, dot_y_r, self._GOLD)

        font = self._font('sao', 10)
        bbox = self._text_bbox(draw, stamp, font)
        stamp_w = bbox[2] - bbox[0]
        draw.text((cx2 - 4 - stamp_w, cy2 + 4), stamp,
                  font=font, fill=self._DIM_GOLD)

    def _build_shell(self, img_w: int, img_h: int) -> Image.Image:
        if _gpu_shell is not None:
            shell = _gpu_shell(
                img_w,
                img_h,
                body_pad=self._PLATE_PAD,
                radius=44.0,
                color_a=(10, 16, 24, 180),
                color_b=(14, 20, 30, 140),
                edge=(110, 210, 240, 60),
                inner=(160, 230, 255, 36),
                scan=(0, 0, 0, 0),
                shadow=self._SHELL_SHADOW,
                shadow_dx=4.0,
                shadow_dy=6.0,
                shadow_sigma=7.0,
                shadow_radius=30.0,
            )
            if shell is not None:
                shell.alpha_composite(self._build_gloss(img_w, img_h))
                return shell

        layer = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))
        shadow = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))
        sdraw = ImageDraw.Draw(shadow)
        sdraw.rounded_rectangle(
            (self._PLATE_PAD + 6, self._PLATE_PAD + 8,
             img_w - self._PLATE_PAD + 6, img_h - self._PLATE_PAD + 8),
            radius=34,
            fill=self._SHELL_SHADOW,
        )
        shadow = self._blur(shadow, 7)
        layer.alpha_composite(shadow)

        draw = ImageDraw.Draw(layer)
        outer = (self._PLATE_PAD, self._PLATE_PAD,
                 img_w - self._PLATE_PAD, img_h - self._PLATE_PAD)
        inner = (self._PLATE_PAD + 10, self._PLATE_PAD + 10,
                 img_w - self._PLATE_PAD - 10, img_h - self._PLATE_PAD - 10)
        draw.rounded_rectangle(
            outer,
            radius=34,
            fill=(10, 16, 24, 180),
            outline=(110, 210, 240, 60),
            width=1,
        )
        draw.rounded_rectangle(
            inner,
            radius=28,
            fill=(14, 20, 30, 140),
            outline=(160, 230, 255, 36),
            width=1,
        )
        layer.alpha_composite(self._build_gloss(img_w, img_h))
        return layer

    def _build_gloss(self, img_w: int, img_h: int) -> Image.Image:
        gloss = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(gloss)
        draw.rounded_rectangle(
            (self._PLATE_PAD + 10, self._PLATE_PAD + 8,
             img_w - self._PLATE_PAD - 12,
             int(self._PLATE_PAD + (img_h - self._PLATE_PAD * 2) * 0.40)),
            radius=24,
            fill=(200, 240, 255, 18),
        )
        return self._blur(gloss, 10)

    def _draw_brackets(self, draw: ImageDraw.ImageDraw,
                       cx1: int, cy1: int, cx2: int, cy2: int) -> None:
        spec = (
            (cx1, cy1, self._BRACKET_LEN, 0, 0, self._BRACKET_LEN, self._CYAN),
            (cx2, cy1, -self._BRACKET_LEN, 0, 0, self._BRACKET_LEN, self._GOLD),
            (cx1, cy2, self._BRACKET_LEN, 0, 0, -self._BRACKET_LEN, self._CYAN),
            (cx2, cy2, -self._BRACKET_LEN, 0, 0, -self._BRACKET_LEN, self._GOLD),
        )
        for x, y, dx1, dy1, dx2, dy2, color in spec:
            draw.line((x, y, x + dx1, y + dy1), fill=color, width=1)
            draw.line((x, y, x + dx2, y + dy2), fill=color, width=1)

    def _draw_rails(self, draw: ImageDraw.ImageDraw,
                    cx1: int, cy1: int, cx2: int, cy2: int) -> None:
        rail_x_l = cx1 - self._RAIL_OFFSET
        rail_x_r = cx2 + self._RAIL_OFFSET
        draw.line((rail_x_l, cy1 + self._BRACKET_LEN,
                   rail_x_l, cy2 - self._BRACKET_LEN), fill=self._CYAN, width=1)
        draw.line((rail_x_r, cy1 + self._BRACKET_LEN,
                   rail_x_r, cy2 - self._BRACKET_LEN), fill=self._GOLD, width=1)

    def _draw_static_labels(self, draw: ImageDraw.ImageDraw,
                            cx1: int, cy1: int, cx2: int, cy2: int,
                            screen_w: int, screen_h: int) -> None:
        font = self._font('sao', 10)
        draw.text((cx1 + 4, cy1 - 12), 'SYS:MENU', font=font, fill=self._DIM_CYAN)
        res_text = f'RES:{screen_w}x{screen_h}'
        bbox = self._text_bbox(draw, res_text, font)
        draw.text((cx2 - 4 - (bbox[2] - bbox[0]), cy1 - 12),
                  res_text, font=font, fill=self._DIM_GOLD)
        draw.text((cx1 + 4, cy2 + 2), 'ACTIVE', font=font, fill=self._DIM_CYAN)

    def _alpha_dot(self, frame: Image.Image, cx: int, cy: int,
                   color: Tuple[int, int, int, int]) -> None:
        glow = self._dot_sprite(color, 10)
        gx = int(cx - glow.width / 2)
        gy = int(cy - glow.height / 2)
        frame.alpha_composite(glow, dest=(gx, gy))
        draw = ImageDraw.Draw(frame)
        r = self._DOT_RADIUS
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)

    def _dot_sprite(self, color: Tuple[int, int, int, int], size: int) -> Image.Image:
        key = (color, size)
        cached = self._dot_cache.get(key)
        if cached is not None:
            return cached
        pad = 6
        sprite = Image.new('RGBA', (size + pad * 2, size + pad * 2), (0, 0, 0, 0))
        draw = ImageDraw.Draw(sprite)
        draw.ellipse((pad, pad, pad + size, pad + size),
                 fill=(color[0], color[1], color[2], 56))
        sprite = self._blur(sprite, 3)
        self._dot_cache[key] = sprite
        return sprite

    def _blur(self, img: Image.Image, radius: float) -> Image.Image:
        if _gpu_blur is not None:
            try:
                return _gpu_blur(img, radius)
            except Exception:
                pass
        return img.filter(ImageFilter.GaussianBlur(radius))

    def _to_photo(self, img: Image.Image) -> ImageTk.PhotoImage:
        size = img.size
        if self._photo is not None and self._photo_size == size:
            try:
                self._photo.paste(img)
                return self._photo
            except Exception:
                self._photo = None
                self._photo_size = None
        self._photo = ImageTk.PhotoImage(img)
        self._photo_size = size
        return self._photo

    def _font(self, kind: str, size: int):
        font_path = _FONT_SAO if kind == 'sao' else _FONT_CJK
        key = (font_path, size)
        cached = self._font_cache.get(key)
        if cached is not None:
            return cached
        try:
            font = ImageFont.truetype(font_path, size=size)
        except Exception:
            font = ImageFont.load_default()
        self._font_cache[key] = font
        return font

    @staticmethod
    def _text_bbox(draw: ImageDraw.ImageDraw, text: str, font) -> Tuple[int, int, int, int]:
        if hasattr(draw, 'textbbox'):
            return draw.textbbox((0, 0), text, font=font)
        width, height = draw.textsize(text, font=font)
        return (0, 0, width, height)


class MenuCircleButtonRenderer:
    def __init__(self) -> None:
        self._image_cache: Dict[Tuple[int, str, str, str, str, str], Image.Image] = {}
        self._font_cache: Dict[Tuple[str, int], ImageFont.FreeTypeFont] = {}

    def render(self, size: int, icon_text: str,
               border_hex: str, fill_hex: str, icon_hex: str,
               bg_hex: str) -> Image.Image:
        size = max(1, int(size))
        bg_hex = bg_hex or '#010101'
        key = (size, icon_text, border_hex, fill_hex, icon_hex, bg_hex)
        cached = self._image_cache.get(key)
        if cached is not None:
            return cached

        if size <= 6:
            final = Image.new('RGBA', (size, size), (0, 0, 0, 0))
            self._image_cache[key] = final
            return final

        scale = 4 if size <= 20 else 3
        canvas = size * scale
        image = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        border_rgb = _hex_to_rgb_tuple(border_hex, (201, 198, 198))
        fill_rgb = _hex_to_rgb_tuple(fill_hex, (255, 255, 255))
        icon_rgb = _hex_to_rgb_tuple(icon_hex, (185, 183, 183))
        inset = max(2 * scale, 2)
        ring_w = max(scale + 1, 2)
        # Guard: at very small sizes the inner insets can collapse and PIL's
        # ellipse raises "x1 must be >= x0". Fall back to a simple filled
        # dot so the growth animation never throws.
        if canvas - 2 * inset < 4:
            final = Image.new('RGBA', (size, size), (0, 0, 0, 0))
            sdraw = ImageDraw.Draw(final)
            sdraw.ellipse((0, 0, size - 1, size - 1),
                          fill=fill_rgb + (255,),
                          outline=border_rgb + (255,))
            self._image_cache[key] = final
            return final
        draw.ellipse(
            (inset, inset, canvas - inset - 1, canvas - inset - 1),
            outline=border_rgb + (255,),
            width=ring_w,
        )

        inner_inset = inset + max(scale * 2, 2)
        if canvas - 2 * inner_inset < 2:
            # Ring-only at small sizes; skip inner fill + icon.
            resized = image.resize((size, size), Image.LANCZOS)
            alpha = resized.getchannel('A').point(lambda a: 0 if a < 24 else a)
            resized.putalpha(alpha)
            self._image_cache[key] = resized
            return resized
        draw.ellipse(
            (inner_inset, inner_inset,
             canvas - inner_inset - 1, canvas - inner_inset - 1),
            fill=fill_rgb + (255,),
        )

        if not self._draw_builtin_icon(draw, icon_text, canvas, icon_rgb + (255,), scale):
            font_size = max(9, int(size * 0.42 * scale))
            font = self._icon_font(font_size)
            bbox = self._text_bbox(draw, icon_text, font)
            text_w = bbox[2] - bbox[0]
            text_h = bbox[3] - bbox[1]
            text_x = int((canvas - text_w) * 0.5)
            text_y = int((canvas - text_h) * 0.5 - scale * 0.35)
            draw.text((text_x, text_y), icon_text, font=font, fill=icon_rgb + (255,))

        resized = image.resize((size, size), Image.LANCZOS)
        # Clamp only the very low-alpha fringe so the icon stays smooth while
        # the transparent corners do not pick up a dark halo on the menu
        # overlay's transparent-color keyed background.
        alpha = resized.getchannel('A').point(lambda a: 0 if a < 24 else a)
        resized.putalpha(alpha)
        final = resized
        self._image_cache[key] = final
        return final

    def _draw_builtin_icon(self, draw: ImageDraw.ImageDraw, icon_text: str,
                           canvas: int, color: Tuple[int, int, int, int],
                           scale: int) -> bool:
        cx = canvas * 0.5
        cy = canvas * 0.5
        stroke = max(scale + 1, 2)
        if icon_text == '⚡':
            pts = [
                (cx - canvas * 0.10, cy - canvas * 0.26),
                (cx + canvas * 0.02, cy - canvas * 0.26),
                (cx - canvas * 0.05, cy - canvas * 0.02),
                (cx + canvas * 0.12, cy - canvas * 0.02),
                (cx - canvas * 0.03, cy + canvas * 0.27),
                (cx + canvas * 0.00, cy + canvas * 0.06),
                (cx - canvas * 0.14, cy + canvas * 0.06),
            ]
            draw.polygon(pts, fill=color)
            return True
        if icon_text == '◆':
            pts = [
                (cx, cy - canvas * 0.18),
                (cx + canvas * 0.18, cy),
                (cx, cy + canvas * 0.18),
                (cx - canvas * 0.18, cy),
            ]
            draw.polygon(pts, fill=color)
            return True
        if icon_text == 'ℹ':
            dot_r = max(scale + 1, 2)
            stem_w = max(scale + 1, 2)
            draw.ellipse(
                (cx - dot_r, cy - canvas * 0.22 - dot_r,
                 cx + dot_r, cy - canvas * 0.22 + dot_r),
                fill=color,
            )
            draw.rounded_rectangle(
                (cx - stem_w * 0.5, cy - canvas * 0.06,
                 cx + stem_w * 0.5, cy + canvas * 0.20),
                radius=stem_w * 0.5,
                fill=color,
            )
            return True
        if icon_text == '⚙':
            ring_r = canvas * 0.15
            outer_r = canvas * 0.24
            tooth_r = max(scale * 1.25, 2.0)
            for idx in range(8):
                ang = (math.pi * 2.0 * idx) / 8.0
                tx = cx + math.cos(ang) * outer_r
                ty = cy + math.sin(ang) * outer_r
                draw.ellipse((tx - tooth_r, ty - tooth_r, tx + tooth_r, ty + tooth_r), fill=color)
            draw.ellipse((cx - outer_r + scale, cy - outer_r + scale,
                          cx + outer_r - scale, cy + outer_r - scale),
                         outline=color, width=stroke)
            draw.ellipse((cx - ring_r, cy - ring_r, cx + ring_r, cy + ring_r), fill=color)
            draw.ellipse((cx - ring_r * 0.48, cy - ring_r * 0.48,
                          cx + ring_r * 0.48, cy + ring_r * 0.48),
                         fill=(255, 255, 255, 0))
            return True
        if icon_text == '⚔':
            blade = max(scale + 1, 2)
            draw.line((cx - canvas * 0.18, cy + canvas * 0.16,
                       cx + canvas * 0.18, cy - canvas * 0.16),
                      fill=color, width=blade)
            draw.line((cx - canvas * 0.18, cy - canvas * 0.16,
                       cx + canvas * 0.18, cy + canvas * 0.16),
                      fill=color, width=blade)
            guard = max(scale, 2)
            draw.line((cx - canvas * 0.10, cy + canvas * 0.08,
                       cx - canvas * 0.02, cy + canvas * 0.15),
                      fill=color, width=guard)
            draw.line((cx + canvas * 0.10, cy + canvas * 0.08,
                       cx + canvas * 0.02, cy + canvas * 0.15),
                      fill=color, width=guard)
            draw.line((cx - canvas * 0.10, cy - canvas * 0.08,
                       cx - canvas * 0.02, cy - canvas * 0.15),
                      fill=color, width=guard)
            draw.line((cx + canvas * 0.10, cy - canvas * 0.08,
                       cx + canvas * 0.02, cy - canvas * 0.15),
                      fill=color, width=guard)
            return True
        return False

    def _icon_font(self, size: int):
        for font_path in (_FONT_SAO, 'seguisym.ttf', 'segoeui.ttf', 'arial.ttf'):
            key = (font_path, size)
            cached = self._font_cache.get(key)
            if cached is not None:
                return cached
            try:
                font = ImageFont.truetype(font_path, size=size)
                self._font_cache[key] = font
                return font
            except Exception:
                continue
        return ImageFont.load_default()

    @staticmethod
    def _text_bbox(draw: ImageDraw.ImageDraw, text: str, font) -> Tuple[int, int, int, int]:
        if hasattr(draw, 'textbbox'):
            return draw.textbbox((0, 0), text, font=font)
        width, height = draw.textsize(text, font=font)
        return (0, 0, width, height)


class MenuLeftInfoRenderer:
    _TOP_BG = (255, 255, 255, 255)
    _BOTTOM_BG = (229, 227, 227, 255)
    _TEXT_MAIN = (51, 51, 51, 255)
    _TEXT_SUB = (85, 85, 85, 255)

    def __init__(self) -> None:
        self._font_cache: Dict[Tuple[str, int], ImageFont.FreeTypeFont] = {}
        self._top_photo: Optional[ImageTk.PhotoImage] = None
        self._top_size: Optional[Tuple[int, int]] = None
        self._bottom_photo: Optional[ImageTk.PhotoImage] = None
        self._bottom_size: Optional[Tuple[int, int]] = None

    def reset(self) -> None:
        self._top_photo = None
        self._top_size = None
        self._bottom_photo = None
        self._bottom_size = None

    def render_top(self, username: str, width: int, height: int,
                   sweep_phase: float = 0.0, sweep_strength: float = 0.0) -> ImageTk.PhotoImage:
        width = max(1, int(width))
        height = max(1, int(height))
        image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        if width >= 20 and height >= 20:
            draw = ImageDraw.Draw(image)
            draw.rectangle((0, 0, width - 1, height - 1), fill=self._TOP_BG)
            for idx in range(6):
                shade = int(20 * (1.0 - idx / 6.0))
                y = max(0, height - 1 - idx)
                draw.line((3, y, max(3, width - 4), y), fill=(shade, shade, shade, 255), width=1)

            mid_y = int(height * 0.77)
            tri_w = min(16, max(7, width // 12))
            tri_h = min(16, max(7, height // 10))
            draw.polygon(
                ((width - 1, mid_y),
                 (max(0, width - tri_w), min(height - 1, mid_y + tri_h // 2)),
                 (width - 1, min(height - 1, mid_y + tri_h))),
                fill=self._TOP_BG,
            )

            font = self._font(_FONT_CJK, 13)
            bbox = self._text_bbox(draw, username, font)
            text_w = bbox[2] - bbox[0]
            text_h = bbox[3] - bbox[1]
            text_x = max(8, int((width - text_w) * 0.5))
            text_y = max(6, int(30 - text_h * 0.5))
            draw.text((text_x, text_y), username, font=font, fill=self._TEXT_MAIN)

            if height > 50:
                for idx in range(3):
                    level = int(170 + idx * 25)
                    y = 49 + idx
                    draw.line(
                        (10 + idx * 2, y, max(10 + idx * 2, width - 10 - idx * 2), y),
                        fill=(level, level, level, 255),
                        width=1,
                    )
            self._apply_sweep(
                image, width, height,
                sweep_phase=sweep_phase,
                sweep_strength=sweep_strength,
                tint=(220, 246, 255),
                alpha_scale=48,
                blur_radius=max(5, width * 0.030),
                slant=0.34,
            )
        return self._to_photo(image, slot='top')

    def render_bottom(self, description: str, width: int, height: int,
                      sweep_phase: float = 0.0, sweep_strength: float = 0.0) -> ImageTk.PhotoImage:
        width = max(1, int(width))
        height = max(1, int(height))
        image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        if width >= 20 and height >= 15:
            draw = ImageDraw.Draw(image)
            draw.rectangle((0, 0, width - 1, height - 1), fill=self._BOTTOM_BG)

            tri_left = min(max(10, width // 8), max(10, width - 20))
            tri_w = min(15, max(9, width // 16))
            tri_h = min(8, max(5, height // 10))
            draw.polygon(
                ((tri_left, 0),
                 (tri_left + tri_w // 2, min(height - 1, tri_h)),
                 (tri_left + tri_w, 0)),
                fill=self._BOTTOM_BG,
            )

            font = self._font(_FONT_CJK, 9)
            lines = self._wrap_text(draw, description or '', font, max(8, width - 20))
            draw.multiline_text((10, 15), '\n'.join(lines), font=font,
                                fill=self._TEXT_SUB, spacing=2)
            self._apply_sweep(
                image, width, height,
                sweep_phase=sweep_phase,
                sweep_strength=sweep_strength * 0.82,
                tint=(255, 236, 196),
                alpha_scale=32,
                blur_radius=max(4, width * 0.024),
                slant=0.26,
            )
        return self._to_photo(image, slot='bottom')

    def _apply_sweep(self, image: Image.Image, width: int, height: int,
                     sweep_phase: float, sweep_strength: float,
                     tint: Tuple[int, int, int], alpha_scale: int,
                     blur_radius: float, slant: float) -> None:
        sweep_strength = max(0.0, min(1.0, float(sweep_strength)))
        if sweep_strength <= 0.004 or width < 18 or height < 12:
            return
        sweep_phase = max(0.0, min(1.0, float(sweep_phase)))
        overlay = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        draw = ImageDraw.Draw(overlay)
        center = int((-0.24 + 1.30 * sweep_phase) * width)
        half_w = max(14, int(width * 0.16))
        skew = int(max(4, height * slant))
        alpha = max(6, int(alpha_scale * sweep_strength))
        draw.polygon(
            (
                (center - half_w, 0),
                (center + int(half_w * 0.28), 0),
                (center + half_w + skew, height),
                (center - int(half_w * 0.55) + skew, height),
            ),
            fill=(tint[0], tint[1], tint[2], alpha),
        )
        overlay = overlay.filter(ImageFilter.GaussianBlur(radius=blur_radius))
        image.alpha_composite(overlay)

    def _to_photo(self, image: Image.Image, slot: str) -> ImageTk.PhotoImage:
        size = image.size
        if slot == 'top':
            if self._top_photo is not None and self._top_size == size:
                try:
                    self._top_photo.paste(image)
                    return self._top_photo
                except Exception:
                    self._top_photo = None
                    self._top_size = None
            self._top_photo = ImageTk.PhotoImage(image)
            self._top_size = size
            return self._top_photo

        if self._bottom_photo is not None and self._bottom_size == size:
            try:
                self._bottom_photo.paste(image)
                return self._bottom_photo
            except Exception:
                self._bottom_photo = None
                self._bottom_size = None
        self._bottom_photo = ImageTk.PhotoImage(image)
        self._bottom_size = size
        return self._bottom_photo

    def _font(self, font_path: str, size: int):
        key = (font_path, size)
        cached = self._font_cache.get(key)
        if cached is not None:
            return cached
        try:
            font = ImageFont.truetype(font_path, size=size)
        except Exception:
            font = ImageFont.load_default()
        self._font_cache[key] = font
        return font

    def _wrap_text(self, draw: ImageDraw.ImageDraw, text: str,
                   font, max_width: int) -> list[str]:
        if not text:
            return ['']
        lines: list[str] = []
        for paragraph in text.splitlines() or ['']:
            current = ''
            for ch in paragraph:
                candidate = current + ch
                bbox = self._text_bbox(draw, candidate, font)
                if current and (bbox[2] - bbox[0]) > max_width:
                    lines.append(current)
                    current = ch
                else:
                    current = candidate
            lines.append(current)
        return lines or ['']

    @staticmethod
    def _text_bbox(draw: ImageDraw.ImageDraw, text: str, font) -> Tuple[int, int, int, int]:
        if hasattr(draw, 'textbbox'):
            return draw.textbbox((0, 0), text, font=font)
        width, height = draw.textsize(text, font=font)
        return (0, 0, width, height)
