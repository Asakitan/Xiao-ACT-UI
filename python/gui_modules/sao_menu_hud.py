from __future__ import annotations

import datetime as _dt
import math
import os
import sys
import threading
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageTk
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


# v2.3.3 GIL-safety fix.
#
# PIL._imagingft (FreeType) is NOT internally re-entrant: when two
# overlay-compose worker threads concurrently call ImageDraw.text()
# / textbbox() / ImageFont.truetype(), the C-level FT_Library state
# corrupts and the main thread's tstate is NULLed at the next
# Tcl_DoOneEvent reacquire ("Fatal Python error: PyEval_RestoreThread:
# the function must be called with the GIL held, but the GIL is
# released (the current Python thread state is NULL)").
#
# The trigger that the user reports — clicking a floating menu bar
# button → menu re-layout (resize) → crash — is exactly this race:
# the click changes the menu HUD content_w/content_h, MenuHudOverlay
# enqueues a fresh render_pil() compose with the new size on its
# overlay-compose lane (rebuilds the static layer with FreeType-rendered
# 'SYS:MENU' / 'ACTIVE' labels + stamp text), AND the SAO menu bar
# painter on a different lane is mid-recompose for the fisheye animation
# (FreeType for icon glyphs).  Two lanes in PIL FreeType simultaneously
# corrupt FT state.
#
# Fix: a single module-level RLock serializes every renderer entry point
# that may rasterize text / build a draw context.  Held across entire
# render(), render_pil(), and MenuCircleButtonRenderer.render() calls
# so the FreeType native code never overlaps across threads.  The lock
# is held during GIL-released PIL ops; that's intentional — other Python
# threads can still acquire the GIL while we wait for FreeType, and the
# wait time is bounded by one compose (≈3-30 ms).
#
# RLock so that a renderer entry point can call its own helpers that
# also acquire the lock without deadlocking.
_PIL_DRAW_LOCK: threading.RLock = threading.RLock()

try:
    from render.gpu_renderer import gaussian_blur_rgba as _gpu_blur
    from render.gpu_renderer import render_shell_rgba as _gpu_shell
except Exception:
    _gpu_blur = None
    _gpu_shell = None

from utils.perf_probe import probe as _probe


_BASE = (
    getattr(sys, '_MEIPASS', os.path.dirname(sys.executable))
    if getattr(sys, 'frozen', False)
    # Round 61 of sao_gui split refactor: this file moved from the project
    # root into gui_modules/, so __file__ now lives one level deeper. Use
    # dirname twice to keep the dev-mode fallback pointing at sao_auto/.
    else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
# v2.1.2-f: 优先使用 config 的 lifted-out 路径解析器, 兼容
# build_release.bat 把 assets/ 提升到 EXE 顶层的模块化布局.
try:
    from config import FONTS_DIR as _CFG_FONTS_DIR
    _FONTS_DIR = _CFG_FONTS_DIR
except Exception:
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


def _unpremultiply_rgba(img: Image.Image) -> Image.Image:
    # Convert a premultiplied-alpha RGBA image to straight alpha.
    #
    # ``render_shell_rgba``'s GPU shader writes premultiplied output (its RGB
    # channels are already scaled by alpha, the convention its own blend
    # pipeline expects), but every PIL-side consumer downstream —
    # ``Image.alpha_composite``, plain ``ImageDraw`` fills for the brackets/
    # rails drawn on top — assumes straight (non-premultiplied) alpha like
    # the rest of this module's PIL fallback path. Left unconverted, any
    # color drawn through this path reads darker/muddier the more transparent
    # it is (e.g. white at alpha 180/255 renders as ~176 gray instead of
    # ~249 near-white) instead of the requested color.
    arr = np.asarray(img).astype(np.float32)
    alpha = arr[..., 3:4]
    safe_alpha = np.where(alpha > 0, alpha, 1.0)
    rgb = np.clip(arr[..., :3] * (255.0 / safe_alpha), 0, 255)
    out = np.concatenate([rgb, alpha], axis=-1).astype(np.uint8)
    return Image.fromarray(out, 'RGBA')


def _tk_font_spec(kind: str, size: int) -> Tuple:
    # Return a Tk font spec, preferring the installed SAO UI/CJK face so
    # the Canvas stamp looks identical to the PIL-rendered version. Falls
    # back to a system font if sao_sound.get_sao_font is unavailable.
    try:
        if kind == 'sao':
            from utils.sao_sound import get_sao_font as _gs
            return _gs(max(6, int(size)))
        from utils.sao_sound import get_cjk_font as _gc
        return _gc(max(6, int(size)))
    except Exception:
        family = 'Segoe UI' if kind == 'sao' else 'Microsoft YaHei UI'
        return (family, max(6, int(size)))


@dataclass
class MenuHudFrame:
    # Lightweight per-frame descriptor for the menu HUD.
    #
    # Instead of a single composited PhotoImage, this splits the HUD into
    # a static background photo + a handful of dynamic primitives that the
    # caller renders via Canvas-native items (lines, images, text). This
    # keeps the per-frame cost at a few coords()/itemconfigure() calls.

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
    canvas_sig: Tuple[int, ...]


class MenuHudSpriteRenderer:
    _PLATE_PAD = 16
    _HUD_MARGIN = 6
    _BRACKET_LEN = 16
    # Extra canvas margin used only by the GPU-path glass shell
    # (_get_static_layer_gpu/_build_shell), on top of _PLATE_PAD. Before
    # this, the shell's own body_pad was _PLATE_PAD (16), which put the
    # plate's edge *inside* the bracket frame's edge (brackets sit at
    # _PLATE_PAD - _HUD_MARGIN = 10px inset, i.e. 6px further out than
    # the plate) — the backdrop visibly didn't reach the HUD frame it's
    # supposed to back, reading as "too small". This grows the whole
    # sprite canvas so the plate can extend a bit *past* the bracket
    # frame instead (see _shell_body_pad below).
    # v1-v3 (20, 27, 31) chased this by inflating the pad every time it
    # was reported "still too small" — wrong direction. The real bug was
    # in the caller (sao_theme/popup_menu.py::_draw_menu_hud): it cached
    # content_w/content_h once per menu-activate event while reading the
    # content frame's *position* live every tick, so whenever the child
    # bar's animated open/close changed the content frame's real width,
    # the HUD's brackets/plate kept using a stale (often much narrower)
    # width at a live (already-shifted) position — reading as "crooked,
    # doesn't wrap the real content" no matter how big this pad got.
    # That's fixed now (content_w/h read live, same as position).
    #
    # IMPORTANT: gpu_pad (canvas/window margin) and _PLATE_MARGIN (how
    # far the visible glass plate extends past the content edge) are
    # INDEPENDENT knobs. They were NOT independent before — every
    # v1-v3 round (20, 27, 31) grew gpu_pad while _shell_body_pad was
    # defined as ``gpu_pad - <constant>``; since the canvas itself is
    # ``content_w + 2*gpu_pad``, the plate's actual size
    # (``canvas - 2*shell_body_pad``) reduces to
    # ``content_w + 2*<constant>`` — the gpu_pad terms cancel exactly.
    # So growing gpu_pad only ever pushed the bracket/rail frame (whose
    # inset is likewise ``gpu_pad - <constant>``, same cancellation)
    # and the transparent canvas/window further out — it never actually
    # grew the plate at all, which is exactly why "make the plate
    # bigger, independently" kept not working. Fixed by making
    # _PLATE_MARGIN its own constant, unrelated to gpu_pad.
    _SHELL_GROW_PAD = 24  # canvas/window margin only — NOT the plate size
    _RAIL_OFFSET = 4
    # Distance (px) from the real widget content's edge to the glass
    # plate's edge — the one true "how big does the plate look" knob.
    # rail sits at HUD_MARGIN(6) + RAIL_OFFSET(4) = 10px past content;
    # 25 clears that with a clearly visible ring of glass around it.
    _PLATE_MARGIN = 25

    @property
    def gpu_pad(self) -> int:
        # Canvas/window margin (px) for the GPU-path sprite — purely how
        # much transparent room + off-screen slack the render surface has;
        # does not affect the plate's or bracket frame's visible size or
        # position relative to content (see _PLATE_MARGIN for that).
        return self._PLATE_PAD + self._SHELL_GROW_PAD

    @property
    def _shell_body_pad(self) -> int:
        # Inset (px) of the glass plate's edge from the GPU canvas edge.
        # Derived so the plate's actual visible size is
        # ``content + 2*_PLATE_MARGIN`` regardless of gpu_pad — grow
        # _PLATE_MARGIN to make the plate itself bigger; grow gpu_pad only
        # to give the sprite/window more surrounding room.
        return max(0, self.gpu_pad - self._PLATE_MARGIN)
    _DOT_RADIUS = 2
    _CYAN = (94, 184, 202, 255)
    _GOLD = (243, 175, 18, 255)
    _DIM_CYAN = (94, 184, 202, 180)
    _DIM_GOLD = (200, 145, 14, 180)
    _SCAN_TRAIL = ((58, 106, 120, 255), (42, 80, 96, 255))
    _SHELL_SHADOW = (76, 122, 138, 24)
    # Glass backdrop tones — match the menu's own near-white chrome family
    # (SAOColors.CIRCLE_BG '#f7f8f8' / CHILD_BG '#f8f8f8' / INFO_BG '#fbfbfb'
    # / CIRCLE_BORDER '#bcc4ca') instead of an unrelated dark navy slab, so
    # the plate reads as "part of this menu" against the white circle
    # buttons and child list rather than a mismatched dark island.
    _SHELL_FILL_OUTER = (247, 248, 248, 200)
    _SHELL_FILL_INNER = (251, 251, 251, 160)
    _SHELL_EDGE_OUTER = (188, 196, 202, 90)
    _SHELL_EDGE_INNER = (255, 255, 255, 80)

    def __init__(self) -> None:
        self._static_key: Optional[Tuple[int, int, int, int]] = None
        self._static_img: Optional[Image.Image] = None
        self._static_photo: Optional[ImageTk.PhotoImage] = None
        self._static_photo_size: Optional[Tuple[int, int]] = None
        # GPU (render_pil / MenuHudOverlay) path only — kept separate from
        # the legacy chroma-key static layer above so the glass backdrop
        # shell never leaks into the transparent-only Canvas fallback path
        # (that path renders on a -transparentcolor window, where a
        # translucent plate produces a visible black fringe; the GPU path
        # is a real per-pixel-alpha layered window and doesn't have that
        # problem — see MenuHudSpriteRenderer._get_static_layer_gpu).
        self._static_key_gpu: Optional[Tuple[int, int, int, int]] = None
        self._static_img_gpu: Optional[Image.Image] = None
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
        # v2.2.25: reusable scratch buffer for render_pil. Avoids a
        # ~840 KB Image alloc + memcpy per frame at 60 Hz; we re-blit
        # the cached static into it via the C-implemented Image.paste.
        self._scratch: Optional[Image.Image] = None
        self._scratch_size: Optional[Tuple[int, int]] = None
        # v2.2.25: pre-rendered stamp sprite (text only), invalidated when
        # the second changes. Avoids font rasterization every frame.
        self._stamp_sprite: Optional[Image.Image] = None
        self._stamp_sprite_text: Optional[str] = None
        self._stamp_sprite_size: Tuple[int, int] = (0, 0)
        self._canvas_frame: Optional[MenuHudFrame] = None
        self._pil_frame_sig: Optional[Tuple[int, ...]] = None
        self._pil_frame_cache: Optional[Image.Image] = None
        self._pil_frame_off: Tuple[int, int] = (0, 0)
        # Pre-rendered scan-sweep sprite (gradient afterglow + hot core),
        # cached by bracket-box width.
        self._scan_sprite: Optional[Image.Image] = None
        self._scan_sprite_w: int = 0
        self._scan_color_hex = _rgba_to_hex(self._CYAN)
        self._trail_colors_hex = (
            _rgba_to_hex(self._SCAN_TRAIL[0]),
            _rgba_to_hex(self._SCAN_TRAIL[1]),
        )
        self._dot_color_l_hex = _rgba_to_hex(self._CYAN)
        self._dot_color_r_hex = _rgba_to_hex(self._GOLD)
        self._stamp_color_hex = _rgba_to_hex(self._DIM_GOLD)
        self._stamp_font_spec = _tk_font_spec('sao', 10)

    def reset(self) -> None:
        self._static_key = None
        self._static_img = None
        self._static_photo = None
        self._static_photo_size = None
        self._static_key_gpu = None
        self._static_img_gpu = None
        self._photo = None
        self._photo_size = None
        self._stamp_second = None
        self._stamp_text = ''
        self._frame_sig = None
        self._scratch = None
        self._scratch_size = None
        self._stamp_sprite = None
        self._stamp_sprite_text = None
        self._stamp_sprite_size = (0, 0)
        self._canvas_frame = None
        self._pil_frame_sig = None
        self._pil_frame_cache = None
        self._pil_frame_off = (0, 0)
        self._scan_sprite = None
        self._scan_sprite_w = 0

    @_probe.decorate('ui.menu.render_canvas')
    def render(self, content_w: int, content_h: int,
               screen_w: int, screen_h: int,
               phase: float) -> 'MenuHudFrame':
        # Return a lightweight frame descriptor that can be drawn with
        # Canvas-native primitives (lines, images, text).
        #
        # The previous implementation composited the full HUD into a single
        # PhotoImage every tick, which required `static.copy()` + PIL draw
        # ops + `PhotoImage.paste()` (~2.7 ms/frame at 60 Hz). By splitting
        # the static background from the dynamic scan/dot/clock elements
        # and letting Tk's Canvas animate them natively, we keep the exact
        # visual output while cutting the per-frame cost ~>25x.
        #
        # Held under :data:`_PIL_DRAW_LOCK`: this entry point can be
        # reached from the Tk main thread (Canvas fallback) while another
        # worker is mid-render_pil — without the lock the two collide in
        # FreeType.
        with _PIL_DRAW_LOCK:
            return self._render_locked(content_w, content_h, screen_w, screen_h, phase)

    def _render_locked(self, content_w: int, content_h: int,
                       screen_w: int, screen_h: int,
                       phase: float) -> 'MenuHudFrame':
        # Same floor as the caller (max(120, ...)) — see the note in
        # _render_pil_locked on why an inflated floor here misaligns the
        # HUD frame against the anchor='se'-placed real content.
        content_w = max(120, int(content_w))
        content_h = max(120, int(content_h))
        screen_w = max(1, int(screen_w))
        screen_h = max(1, int(screen_h))

        cx1, cy1, cx2, cy2, scan_y, dot_y_l, dot_y_r = _CY_UI.popup_hud_dynamic(
            content_w, content_h, phase, self._PLATE_PAD,
            self._HUD_MARGIN, self._BRACKET_LEN)
        now = _dt.datetime.now()
        now_second = int(now.timestamp())
        frame_sig = (
            content_w, content_h, screen_w, screen_h,
            cx1, cy1, cx2, cy2, scan_y, dot_y_l, dot_y_r,
            now_second,
        )
        if frame_sig == self._frame_sig and self._canvas_frame is not None:
            return self._canvas_frame

        static_photo = self._get_static_photo(content_w, content_h,
                                              screen_w, screen_h)
        dot_photo_l = self._dot_photo(self._CYAN, 10)
        dot_photo_r = self._dot_photo(self._GOLD, 10)

        if self._stamp_second != now_second:
            self._stamp_second = now_second
            self._stamp_text = now.strftime('%H:%M:%S')

        frame = MenuHudFrame(
            static_photo=static_photo,
            static_size=self._static_photo_size or (0, 0),
            cx1=cx1, cy1=cy1, cx2=cx2, cy2=cy2,
            rail_x_l=cx1 - self._RAIL_OFFSET,
            rail_x_r=cx2 + self._RAIL_OFFSET,
            scan_y=scan_y,
            trail_ys=(scan_y - 2, scan_y - 4),
            trail_colors=self._trail_colors_hex,
            scan_color=self._scan_color_hex,
            dot_y_l=dot_y_l, dot_y_r=dot_y_r,
            dot_color_l=self._dot_color_l_hex,
            dot_color_r=self._dot_color_r_hex,
            dot_photo_l=dot_photo_l,
            dot_photo_r=dot_photo_r,
            dot_radius=self._DOT_RADIUS,
            dot_glow_size=dot_photo_l.width() if dot_photo_l else 30,
            stamp_text=self._stamp_text,
            stamp_pos=(cx2 - 2, cy2 + 2),   # Tk text anchor='ne'
            stamp_color=self._stamp_color_hex,
            stamp_font=self._stamp_font_spec,
            canvas_sig=(cx1, cy1, cx2, cy2, scan_y,
                        dot_y_l, dot_y_r, now_second),
        )
        self._frame_sig = frame_sig
        self._canvas_frame = frame
        return frame

    @_probe.decorate('ui.menu.render_pil')
    def render_pil(self, content_w: int, content_h: int,
                   screen_w: int, screen_h: int,
                   phase: float) -> Tuple[Image.Image, Tuple[int, int]]:
        # v2.2.12: compose the entire HUD into a single RGBA PIL.Image.
        #
        # Used by ``MenuHudOverlay`` (sao_gui_menu_hud) to drive a layered
        # per-pixel-alpha window from a worker thread instead of the
        # ``Toplevel(-transparentcolor) + Canvas`` chroma-key path. Touches
        # no Tk objects so it is safe to call off the main thread.
        #
        # Returns ``(image, sprite_origin_offset)`` where the offset is the
        # sprite's top-left relative to the content frame's top-left
        # (``-PLATE_PAD, -PLATE_PAD``). The caller adds it to the desired
        # on-screen position before submitting via ``ulw_commit``.
        #
        # Held under :data:`_PIL_DRAW_LOCK` — see module docstring.
        with _PIL_DRAW_LOCK:
            return self._render_pil_locked(content_w, content_h, screen_w, screen_h, phase)

    def _render_pil_locked(self, content_w: int, content_h: int,
                           screen_w: int, screen_h: int,
                           phase: float) -> Tuple[Image.Image, Tuple[int, int]]:
        # Floor MUST match the caller's (sao_theme/popup_menu.py::
        # _draw_menu_hud passes max(120, cw/ch), and MenuHudOverlay stores
        # max(120, ...)). The old max(260, ...)/max(180, ...) here silently
        # inflated the drawn frame up to 140px wider / whatever taller than
        # the size the *position* was computed from — and since the sprite
        # is placed by the real Tk content frame's anchor='se' geometry
        # (its se corner pinned to the floating button), that extra width
        # pushed the frame's se corner ~140px right/down of the button
        # instead of onto it: the frame rendered "shifted down-right,
        # doesn't wrap the real content, should be further up-left". Same
        # floor as the caller → drawn size == positioned size, always.
        content_w = max(120, int(content_w))
        content_h = max(120, int(content_h))
        screen_w = max(1, int(screen_w))
        screen_h = max(1, int(screen_h))

        cx1, cy1, cx2, cy2, scan_y, dot_y_l, dot_y_r = _CY_UI.popup_hud_dynamic(
            content_w, content_h, phase, self.gpu_pad,
            self._HUD_MARGIN, self._BRACKET_LEN)

        now = _dt.datetime.now()
        now_second = int(now.timestamp())
        frame_sig = (
            content_w, content_h, screen_w, screen_h,
            cx1, cy1, cx2, cy2, scan_y, dot_y_l, dot_y_r,
            now_second,
        )
        # Cache check MUST happen before the scratch buffer is touched:
        # the cached frame IS the scratch object, so blitting the static
        # layer first wipes the scan line / dots / clock off the very
        # image a cache hit is about to return. At 60 Hz the int-quantized
        # scan/dot positions repeat across adjacent frames constantly, so
        # roughly every other presented frame lost its dynamic elements —
        # visible as the scan line / glow dots / timestamp flickering.
        if frame_sig == self._pil_frame_sig and self._pil_frame_cache is not None:
            return self._pil_frame_cache, self._pil_frame_off
        if self._stamp_second != now_second:
            self._stamp_second = now_second
            self._stamp_text = now.strftime('%H:%M:%S')

        static = self._get_static_layer_gpu(
            content_w, content_h, screen_w, screen_h)
        # v2.2.25: reuse a single scratch RGBA image across frames. The
        # previous `static.copy()` allocated a fresh PIL Image (~840 KB
        # for a typical 480×440 menu HUD) every tick; at 60 Hz that's
        # 50 MB/s of pure allocation churn for the menu alone, on top of
        # the actual memcpy. We now keep one scratch buffer and re-blit
        # the cached static into it via the C-implemented Image.paste,
        # which is a straight memcpy with no Python-level allocation.
        sw_size = static.size
        if self._scratch is None or self._scratch_size != sw_size:
            self._scratch = Image.new('RGBA', sw_size, (0, 0, 0, 0))
            self._scratch_size = sw_size
        scratch = self._scratch
        scratch.paste(static, (0, 0))
        frame = scratch

        self._draw_dynamic(frame, cx1, cy1, cx2, cy2,
                           scan_y, dot_y_l, dot_y_r,
                           self._stamp_text)
        self._pil_frame_sig = frame_sig
        self._pil_frame_cache = frame
        self._pil_frame_off = (-self.gpu_pad, -self.gpu_pad)
        return frame, self._pil_frame_off

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
        # Keep this layer transparent-only — it backs the legacy
        # Toplevel(-transparentcolor) chroma-key Canvas fallback, where a
        # translucent plate behind the content produces a visible black
        # fringe around the floating menu and child options. The GPU path
        # (MenuHudOverlay, a real per-pixel-alpha window) gets the glass
        # backdrop instead via _get_static_layer_gpu below.
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

    def _get_static_layer_gpu(self, content_w: int, content_h: int,
                              screen_w: int, screen_h: int) -> Image.Image:
        # Same static HUD layer as :meth:`_get_static_layer`, but with the
        # glass backdrop shell composited underneath — safe here because the
        # GPU overlay (MenuHudOverlay) is a real per-pixel-alpha layered
        # window, not a chroma-key surface, so a translucent plate doesn't
        # produce the black-fringe artifact the legacy path avoids. Uses its
        # own cache slot so this never leaks into the chroma-key fallback.
        key = (content_w, content_h, screen_w, screen_h)
        if self._static_key_gpu == key and self._static_img_gpu is not None:
            return self._static_img_gpu

        gpu_pad = self.gpu_pad
        img_w = content_w + gpu_pad * 2
        img_h = content_h + gpu_pad * 2
        layer = self._build_shell(img_w, img_h, body_pad=self._shell_body_pad)

        draw = ImageDraw.Draw(layer)
        cx1 = gpu_pad - self._HUD_MARGIN
        cy1 = gpu_pad - self._HUD_MARGIN
        cx2 = gpu_pad + content_w + self._HUD_MARGIN
        cy2 = gpu_pad + content_h + self._HUD_MARGIN
        self._draw_brackets(draw, cx1, cy1, cx2, cy2)
        self._draw_rails(draw, cx1, cy1, cx2, cy2)
        self._draw_static_labels(draw, cx1, cy1, cx2, cy2, screen_w, screen_h)

        self._static_key_gpu = key
        self._static_img_gpu = layer
        return layer

    def _draw_dynamic(self, frame: Image.Image,
                      cx1: int, cy1: int, cx2: int, cy2: int,
                      scan_y: int, dot_y_l: int, dot_y_r: int,
                      stamp: str) -> None:
        draw = ImageDraw.Draw(frame)
        # Scan sweep: gradient afterglow band rising into a hot core with
        # a short lead glow beneath, clipped to the bracket box so the
        # glow never spills past the top/bottom frame lines.
        sweep = self._get_scan_sprite(max(1, cx2 - cx1))
        top = scan_y - self._SCAN_TRAIL_H
        y0 = max(top, cy1 + 1)
        y1 = min(top + sweep.height, cy2)
        if y1 > y0:
            if y0 > top or y1 < top + sweep.height:
                sweep = sweep.crop((0, y0 - top, sweep.width, y1 - top))
            frame.alpha_composite(sweep, (cx1, y0))
        # End ticks where the sweep core meets the rails.
        draw.line((cx1, scan_y - 2, cx1, scan_y + 2), fill=self._CYAN, width=1)
        draw.line((cx2, scan_y - 2, cx2, scan_y + 2), fill=self._CYAN, width=1)

        rail_x_l = cx1 - self._RAIL_OFFSET
        rail_x_r = cx2 + self._RAIL_OFFSET
        self._alpha_dot(frame, rail_x_l, dot_y_l, self._CYAN)
        self._alpha_dot(frame, rail_x_r, dot_y_r, self._GOLD)

        # v2.2.25: stamp text changes only once per second. Cache it as
        # a tiny sprite and paste it each frame, avoiding the FreeType
        # rasterization + textbbox + ImageDraw.text path on every tick.
        sprite = self._get_stamp_sprite(stamp)
        if sprite is not None:
            sx = cx2 - 4 - self._stamp_sprite_size[0]
            sy = cy2 + 4
            frame.alpha_composite(sprite, dest=(sx, sy))

    _SCAN_TRAIL_H = 26   # afterglow band height above the sweep core
    _SCAN_LEAD_H = 5     # faint lead glow below the core

    def _get_scan_sprite(self, width: int) -> Image.Image:
        # Scan-sweep sprite: a vertical gradient afterglow that rises
        # into a 2px hot core (near-white leading edge) with a short lead
        # glow beneath. Built once per bracket-box width and cached — the
        # per-frame cost is a single C-level alpha_composite.
        if self._scan_sprite is not None and self._scan_sprite_w == width:
            return self._scan_sprite
        r, g, b = self._CYAN[:3]
        h = self._SCAN_TRAIL_H + 2 + self._SCAN_LEAD_H
        col = np.zeros((h, 4), dtype=np.uint8)
        for i in range(self._SCAN_TRAIL_H):
            t = (i + 1) / self._SCAN_TRAIL_H
            col[i] = (r, g, b, int(80 * t ** 1.7))
        col[self._SCAN_TRAIL_H] = (176, 228, 242, 235)
        col[self._SCAN_TRAIL_H + 1] = (r, g, b, 150)
        for i in range(self._SCAN_LEAD_H):
            t = 1.0 - (i + 1) / self._SCAN_LEAD_H
            col[self._SCAN_TRAIL_H + 2 + i] = (r, g, b, int(46 * t ** 1.5))
        arr = np.repeat(col[:, None, :], width, axis=1)
        self._scan_sprite = Image.fromarray(arr, 'RGBA')
        self._scan_sprite_w = width
        return self._scan_sprite

    def _get_stamp_sprite(self, stamp: str) -> Optional[Image.Image]:
        if not stamp:
            return None
        if (self._stamp_sprite is not None
                and self._stamp_sprite_text == stamp):
            return self._stamp_sprite
        font = self._font('sao', 10)
        # Use a throwaway image to measure first.
        probe = Image.new('RGBA', (1, 1), (0, 0, 0, 0))
        bbox = self._text_bbox(ImageDraw.Draw(probe), stamp, font)
        w = max(1, bbox[2] - bbox[0])
        h = max(1, bbox[3] - bbox[1])
        sprite = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(sprite).text(
            (-bbox[0], -bbox[1]), stamp, font=font, fill=self._DIM_GOLD)
        self._stamp_sprite = sprite
        self._stamp_sprite_text = stamp
        self._stamp_sprite_size = (w, h)
        return sprite

    def _build_shell(self, img_w: int, img_h: int, body_pad: Optional[int] = None) -> Image.Image:
        pad = self._PLATE_PAD if body_pad is None else body_pad
        if _gpu_shell is not None:
            shell = _gpu_shell(
                img_w,
                img_h,
                body_pad=pad,
                radius=44.0,
                color_a=self._SHELL_FILL_OUTER,
                color_b=self._SHELL_FILL_INNER,
                edge=self._SHELL_EDGE_OUTER,
                inner=self._SHELL_EDGE_INNER,
                scan=(0, 0, 0, 0),
                # Shadow extent (dy + ~2*sigma) must stay inside the
                # canvas margin left outside the plate (_shell_body_pad,
                # 15px) — a larger reach ran off the image's right/bottom
                # edge and got hard-truncated there, reading as a clipped
                # / squared-off bottom-right corner while the top-left
                # (where the shadow tucks under the plate) stayed soft.
                shadow=self._SHELL_SHADOW,
                shadow_dx=2.0,
                shadow_dy=3.0,
                shadow_sigma=4.0,
                shadow_radius=30.0,
            )
            if shell is not None:
                shell = _unpremultiply_rgba(shell)
                shell.alpha_composite(self._build_gloss(img_w, img_h))
                return shell

        layer = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))
        shadow = Image.new('RGBA', (img_w, img_h), (0, 0, 0, 0))
        sdraw = ImageDraw.Draw(shadow)
        sdraw.rounded_rectangle(
            (pad + 2, pad + 3,
             img_w - pad + 2, img_h - pad + 3),
            radius=34,
            fill=self._SHELL_SHADOW,
        )
        shadow = self._blur(shadow, 4)
        layer.alpha_composite(shadow)

        draw = ImageDraw.Draw(layer)
        outer = (pad, pad,
                 img_w - pad, img_h - pad)
        inner = (pad + 10, pad + 10,
                 img_w - pad - 10, img_h - pad - 10)
        draw.rounded_rectangle(
            outer,
            radius=34,
            fill=self._SHELL_FILL_OUTER,
            outline=self._SHELL_EDGE_OUTER,
            width=1,
        )
        draw.rounded_rectangle(
            inner,
            radius=28,
            fill=self._SHELL_FILL_INNER,
            outline=self._SHELL_EDGE_INNER,
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
            fill=(255, 255, 255, 18),
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

    def _blur(self, img: Image.Image, radius: float) -> Image.Image:
        if _gpu_blur is not None:
            try:
                return _gpu_blur(img, radius)
            except Exception:
                pass
        return img.filter(ImageFilter.GaussianBlur(radius))

    def render(self, size: int, icon_text: str,
               border_hex: str, fill_hex: str, icon_hex: str,
               bg_hex: str) -> Image.Image:
        # Held under module-level :data:`_PIL_DRAW_LOCK` so the icon's
        # FreeType rasterization (draw.text inside the !cache-miss
        # branch) doesn't race the menu HUD overlay-compose worker also
        # in FreeType. Cache hits skip the lock cost (single attr read).
        with _PIL_DRAW_LOCK:
            return self._render_locked(size, icon_text, border_hex, fill_hex, icon_hex, bg_hex)

    def _render_locked(self, size: int, icon_text: str,
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

        # Soft drop shadow behind the ring for a little depth — kept inside
        # the existing (size,size) canvas (no expansion) so both callers
        # (SAOCircleButton's subpixel composite and MenuBarGpuPainter's
        # slot placement) keep positioning the sprite exactly as before.
        # Both render paths sit on the real per-pixel-alpha GpuOverlayWindow
        # (confirmed for MenuHudOverlay and MenuBarGpuPainter), not a
        # chroma-key surface, so this soft alpha is safe here.
        dy = max(2, size * 0.06) * scale
        blur_r = max(2, size * 0.10) * scale
        shadow_layer = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
        ImageDraw.Draw(shadow_layer).ellipse(
            (inset, inset + dy, canvas - inset - 1, canvas - inset - 1 + dy),
            fill=(76, 122, 138, 26),
        )
        image.alpha_composite(self._blur(shadow_layer, blur_r))

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
        compact = canvas <= 96
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
        # Script world clock: layered clock face with a small world meridian.
        if icon_text == '◷':
            r = canvas * 0.235
            tick_w = max(1, stroke - scale)
            draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=color, width=stroke)
            draw.arc((cx - r * 0.72, cy - r * 0.72, cx + r * 0.72, cy + r * 0.72),
                     205, 335, fill=color, width=tick_w)
            tick_count = 4 if compact else 12
            for idx in range(tick_count):
                ang = -math.pi / 2.0 + idx * (math.pi * 2.0 / tick_count)
                is_cardinal = compact or idx % 3 == 0
                inner = r * (0.64 if is_cardinal else 0.74)
                outer = r * 0.88
                width = max(1, tick_w + (1 if is_cardinal else 0))
                draw.line((cx + math.cos(ang) * inner, cy + math.sin(ang) * inner,
                           cx + math.cos(ang) * outer, cy + math.sin(ang) * outer),
                          fill=color, width=width)
            draw.line((cx, cy, cx + r * 0.02, cy - r * 0.58), fill=color, width=stroke)
            draw.line((cx, cy, cx + r * 0.50, cy + r * 0.18), fill=color, width=stroke)
            dot = max(scale * 1.15, 2)
            draw.ellipse((cx - dot, cy - dot, cx + dot, cy + dot), fill=color)
            globe_r = r * (0.31 if compact else 0.34)
            gx = cx + r * 0.82
            gy = cy + r * 0.72
            draw.ellipse((gx - globe_r, gy - globe_r, gx + globe_r, gy + globe_r),
                         outline=color, width=tick_w)
            if not compact:
                draw.arc((gx - globe_r, gy - globe_r * 0.52, gx + globe_r, gy + globe_r * 0.52),
                         180, 360, fill=color, width=tick_w)
            draw.line((gx - globe_r * 0.56, gy, gx + globe_r * 0.56, gy),
                      fill=color, width=tick_w)
            draw.line((gx, gy - globe_r, gx, gy + globe_r), fill=color, width=tick_w)
            return True
        # Script 3D stickwoman: posed model silhouette on a tiny stage base.
        if icon_text == '♀':
            base_w = canvas * 0.34
            base_y = cy + canvas * 0.265
            base_h = canvas * 0.075
            base = [
                (cx, base_y - base_h),
                (cx + base_w, base_y),
                (cx, base_y + base_h),
                (cx - base_w, base_y),
            ]
            draw.line(base + [base[0]], fill=color, width=max(1, stroke - scale))
            head_r = canvas * 0.072
            head_y = cy - canvas * 0.205
            hair_r = head_r * 1.22
            draw.arc((cx - hair_r, head_y - hair_r * 1.05,
                      cx + hair_r, head_y + hair_r * 1.18),
                     190, 350, fill=color, width=max(1, stroke - scale))
            draw.ellipse((cx - head_r * 0.82, head_y - head_r * 0.72,
                          cx + head_r * 0.82, head_y + head_r * 0.82),
                         fill=color)
            neck_y = head_y + head_r * 1.25
            hip_y = cy + canvas * 0.085
            shoulder_y = neck_y + canvas * 0.05
            draw.line((cx, neck_y, cx, hip_y), fill=color, width=max(stroke, scale * 2))
            draw.line((cx - canvas * 0.17, shoulder_y,
                       cx + canvas * 0.17, shoulder_y), fill=color, width=stroke)
            draw.line((cx - canvas * 0.15, shoulder_y,
                       cx - canvas * 0.25, cy + canvas * 0.01), fill=color, width=stroke)
            draw.line((cx + canvas * 0.15, shoulder_y,
                       cx + canvas * 0.23, cy - canvas * 0.13), fill=color, width=stroke)
            draw.line((cx - canvas * 0.07, hip_y, cx - canvas * 0.18, cy + canvas * 0.25),
                      fill=color, width=stroke)
            draw.line((cx + canvas * 0.07, hip_y, cx + canvas * 0.14, cy + canvas * 0.25),
                      fill=color, width=stroke)
            hand_r = max(scale * 1.0, 2)
            draw.ellipse((cx + canvas * 0.23 - hand_r, cy - canvas * 0.13 - hand_r,
                          cx + canvas * 0.23 + hand_r, cy - canvas * 0.13 + hand_r), fill=color)
            foot = max(scale, 2)
            draw.line((cx - canvas * 0.205, cy + canvas * 0.25,
                       cx - canvas * 0.105, cy + canvas * 0.25), fill=color, width=foot)
            draw.line((cx + canvas * 0.095, cy + canvas * 0.25,
                       cx + canvas * 0.195, cy + canvas * 0.25), fill=color, width=foot)
            joint_r = max(scale * 0.8, 2)
            for jx, jy in (
                (cx - canvas * 0.15, shoulder_y),
                (cx + canvas * 0.15, shoulder_y),
                (cx - canvas * 0.18, cy + canvas * 0.25),
                (cx + canvas * 0.14, cy + canvas * 0.25),
            ):
                draw.ellipse((jx - joint_r, jy - joint_r, jx + joint_r, jy + joint_r),
                             fill=color)
            return True
        # Script Flappy: bird in motion with a tiny pipe silhouette.
        if icon_text == '◥':
            pipe_w = canvas * 0.07
            px = cx + canvas * 0.275
            draw.rounded_rectangle((px, cy - canvas * 0.29, px + pipe_w, cy - canvas * 0.11),
                                   radius=max(1, scale), fill=color)
            draw.rounded_rectangle((px, cy + canvas * 0.11, px + pipe_w, cy + canvas * 0.29),
                                   radius=max(1, scale), fill=color)
            body = (cx - canvas * 0.22, cy - canvas * 0.105,
                    cx + canvas * 0.14, cy + canvas * 0.15)
            draw.ellipse(body, fill=color)
            wing = [
                (cx - canvas * 0.13, cy + canvas * 0.01),
                (cx - canvas * 0.035, cy - canvas * 0.25),
                (cx + canvas * 0.09, cy + canvas * 0.02),
                (cx - canvas * 0.035, cy + canvas * 0.10),
            ]
            draw.polygon(wing, outline=color, fill=(color[0], color[1], color[2], max(96, color[3] // 2)))
            beak = [
                (cx + canvas * 0.125, cy - canvas * 0.02),
                (cx + canvas * 0.245, cy - canvas * 0.06),
                (cx + canvas * 0.145, cy + canvas * 0.07),
            ]
            draw.polygon(beak, fill=color)
            tail = [
                (cx - canvas * 0.205, cy + canvas * 0.00),
                (cx - canvas * 0.31, cy - canvas * 0.09),
                (cx - canvas * 0.275, cy + canvas * 0.105),
            ]
            draw.polygon(tail, fill=color)
            eye = max(scale * 1.0, 2)
            draw.ellipse((cx + canvas * 0.060 - eye, cy - canvas * 0.050 - eye,
                          cx + canvas * 0.060 + eye, cy - canvas * 0.050 + eye),
                         fill=(0, 0, 0, 0))
            return True
        # Script Snake: board, food, and a readable coiled path.
        if icon_text == '▣':
            board_r = canvas * 0.235
            draw.rounded_rectangle((cx - board_r, cy - board_r, cx + board_r, cy + board_r),
                                   radius=max(scale * 3, 3), outline=color,
                                   width=max(1, stroke - scale))
            grid_w = max(1, scale)
            grid_offsets = () if compact else (-board_r * 0.35, board_r * 0.35)
            for off in grid_offsets:
                draw.line((cx - board_r, cy + off, cx + board_r, cy + off),
                          fill=color, width=grid_w)
                draw.line((cx + off, cy - board_r, cx + off, cy + board_r),
                          fill=color, width=grid_w)
            step = canvas * 0.088
            pts = [
                (cx - step * 1.9, cy - step * 1.15),
                (cx + step * 1.15, cy - step * 1.15),
                (cx + step * 1.15, cy + step * 0.05),
                (cx - step * 1.15, cy + step * 0.05),
                (cx - step * 1.15, cy + step * 1.28),
                (cx + step * 1.75, cy + step * 1.28),
            ]
            draw.line(pts, fill=color, width=max(stroke + scale, 4), joint='curve')
            head_r = max(scale * 2.0, 3)
            hx, hy = pts[-1]
            draw.ellipse((hx - head_r, hy - head_r, hx + head_r, hy + head_r), fill=color)
            food_r = max(scale * 1.7, 3)
            fx = cx - board_r * 0.55
            fy = cy + board_r * 0.58
            draw.ellipse((fx - food_r, fy - food_r, fx + food_r, fy + food_r),
                         fill=color)
            tongue = max(scale, 1)
            draw.line((hx + head_r * 0.6, hy, hx + head_r * 1.6, hy - tongue),
                      fill=color, width=max(1, scale))
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
        # Palette / theme icon: ellipse body with thumb hole + 3 dots
        if icon_text == 'P':
            r = canvas * 0.20
            # Main palette body (offset slightly left-up)
            px = cx - canvas * 0.01
            py = cy + canvas * 0.01
            draw.ellipse((px - r, py - r, px + r, py + r), fill=color)
            # Thumb hole
            hr = r * 0.32
            draw.ellipse((px - hr, py + r * 0.25 - hr,
                          px + hr, py + r * 0.25 + hr),
                         fill=(0, 0, 0, 0))
            # Color dots
            dr = max(scale * 1.1, 2)
            draw.ellipse((cx - r * 0.50 - dr, cy - r * 0.55 - dr,
                          cx - r * 0.50 + dr, cy - r * 0.55 + dr),
                         fill=(255, 255, 255, 0))  # negative space
            draw.ellipse((cx - r * 0.50 - dr, cy - r * 0.55 - dr,
                          cx - r * 0.50 + dr, cy - r * 0.55 + dr),
                         outline=color, width=max(scale, 1))
            draw.ellipse((cx + r * 0.20 - dr, cy - r * 0.65 - dr,
                          cx + r * 0.20 + dr, cy - r * 0.65 + dr),
                         outline=color, width=max(scale, 1))
            draw.ellipse((cx + r * 0.45 - dr, cy + r * 0.10 - dr,
                          cx + r * 0.45 + dr, cy + r * 0.10 + dr),
                         outline=color, width=max(scale, 1))
            return True
        # Plugin / module icon: a filled hexagon (nut/module) with a hollow
        # center, distinct from the ◆ diamond. SAOUI.ttf lacks ⬢ (U+2B22) so it
        # MUST be drawn here — the circle-button font path has no glyph fallback.
        if icon_text == '⬢':
            r = canvas * 0.22
            pts = []
            for idx in range(6):
                ang = math.pi / 6.0 + idx * (math.pi / 3.0)  # flat-top hexagon
                pts.append((cx + math.cos(ang) * r, cy + math.sin(ang) * r))
            draw.polygon(pts, fill=color)
            hr = r * 0.40
            draw.ellipse((cx - hr, cy - hr, cx + hr, cy + hr), fill=(255, 255, 255, 0))
            return True
        # AI Editor: four-pointed star (✦ U+2726)
        if icon_text == '✦':
            r = canvas * 0.24
            ri = r * 0.32
            pts = []
            for idx in range(4):
                ang_o = -math.pi / 2.0 + idx * (math.pi / 2.0)
                ang_i = ang_o + math.pi / 4.0
                pts.append((cx + math.cos(ang_o) * r, cy + math.sin(ang_o) * r))
                pts.append((cx + math.cos(ang_i) * ri, cy + math.sin(ang_i) * ri))
            draw.polygon(pts, fill=color)
            return True
        # Workshop diamond outline (◇ U+25C7) — "创意工坊" category
        if icon_text == '◇':
            r = canvas * 0.22
            ri = r * 0.62
            pts_outer = [
                (cx, cy - r), (cx + r, cy),
                (cx, cy + r), (cx - r, cy),
            ]
            pts_inner = [
                (cx, cy - ri), (cx + ri, cy),
                (cx, cy + ri), (cx - ri, cy),
            ]
            draw.polygon(pts_outer, fill=color)
            draw.polygon(pts_inner, fill=(0, 0, 0, 0))
            # center sparkle dot
            dr = max(scale * 0.8, 1.5)
            draw.ellipse((cx - dr, cy - dr, cx + dr, cy + dr), fill=color)
            return True
        # Hash / number sign (⌗ U+2317) — "工具" category
        if icon_text == '⌗':
            bar = max(scale, 2)
            span = canvas * 0.18
            gap = span * 0.45
            # two vertical bars
            draw.line((cx - gap, cy - span, cx - gap, cy + span), fill=color, width=bar)
            draw.line((cx + gap, cy - span, cx + gap, cy + span), fill=color, width=bar)
            # two horizontal bars (slightly tilted via offset)
            draw.line((cx - span, cy - gap, cx + span, cy - gap), fill=color, width=bar)
            draw.line((cx - span, cy + gap, cx + span, cy + gap), fill=color, width=bar)
            return True
        # Script CuteGirl desktop pet: cat-ear character silhouette.
        if icon_text == '🐾':
            head_r = canvas * 0.14
            head_y = cy - canvas * 0.04
            draw.ellipse((cx - head_r, head_y - head_r, cx + head_r, head_y + head_r),
                         fill=color)
            ear_h = canvas * 0.13
            ear_w = canvas * 0.09
            draw.polygon([
                (cx - head_r * 0.85, head_y - head_r * 0.55),
                (cx - head_r * 0.45, head_y - head_r - ear_h),
                (cx - head_r * 0.05, head_y - head_r * 0.30),
            ], fill=color)
            draw.polygon([
                (cx + head_r * 0.05, head_y - head_r * 0.30),
                (cx + head_r * 0.45, head_y - head_r - ear_h),
                (cx + head_r * 0.85, head_y - head_r * 0.55),
            ], fill=color)
            body_top = head_y + head_r * 0.6
            body_bot = cy + canvas * 0.25
            body_w = canvas * 0.12
            draw.rounded_rectangle(
                (cx - body_w, body_top, cx + body_w, body_bot),
                radius=max(scale * 2, 3), fill=color)
            leg_w = max(stroke, scale * 2)
            draw.line((cx - body_w * 0.6, body_bot, cx - body_w * 0.6, body_bot + canvas * 0.07),
                      fill=color, width=leg_w)
            draw.line((cx + body_w * 0.6, body_bot, cx + body_w * 0.6, body_bot + canvas * 0.07),
                      fill=color, width=leg_w)
            eye_r = max(scale * 0.9, 1.5)
            draw.ellipse((cx - head_r * 0.45 - eye_r, head_y - eye_r,
                          cx - head_r * 0.45 + eye_r, head_y + eye_r),
                         fill=(0, 0, 0, 0))
            draw.ellipse((cx + head_r * 0.45 - eye_r, head_y - eye_r,
                          cx + head_r * 0.45 + eye_r, head_y + eye_r),
                         fill=(0, 0, 0, 0))
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
    _TOP_BG = (251, 251, 251, 255)
    _BOTTOM_BG = (236, 235, 234, 255)
    _TEXT_MAIN = (100, 99, 100, 255)
    _TEXT_SUB = (140, 135, 138, 255)

    def __init__(self) -> None:
        self._font_cache: Dict[Tuple[str, int], ImageFont.FreeTypeFont] = {}
        self._top_photo: Optional[ImageTk.PhotoImage] = None
        self._top_size: Optional[Tuple[int, int]] = None
        self._bottom_photo: Optional[ImageTk.PhotoImage] = None
        self._bottom_size: Optional[Tuple[int, int]] = None
        # v2.2.26: static plate cache (no sweep). Open/close + sync_pulse
        # animations vary the panel size every frame; without caching the
        # body, every frame re-runs ImageDraw.rectangle/lines/text/triangle
        # plus a fresh PIL alloc. Now we keep one body per (username,w,h)
        # and a separate sig-keyed scratch for sweep composites so steady
        # frames return the cached PhotoImage immediately.
        self._top_body_cache: Dict[Tuple[str, int, int], Image.Image] = {}
        self._bottom_body_cache: Dict[Tuple[str, int, int], Image.Image] = {}
        self._top_sig: Optional[Tuple] = None
        self._bottom_sig: Optional[Tuple] = None
        self._top_pil_sig: Optional[Tuple] = None
        self._bottom_pil_sig: Optional[Tuple] = None
        self._top_pil_frame: Optional[Image.Image] = None
        self._bottom_pil_frame: Optional[Image.Image] = None

    def reset(self) -> None:
        self._top_photo = None
        self._top_size = None
        self._bottom_photo = None
        self._bottom_size = None
        self._top_body_cache.clear()
        self._bottom_body_cache.clear()
        self._top_sig = None
        self._bottom_sig = None
        self._top_pil_sig = None
        self._bottom_pil_sig = None
        self._top_pil_frame = None
        self._bottom_pil_frame = None

    @_probe.decorate('ui.menu.plate_top')
    def render_top(self, username: str, width: int, height: int,
                   sweep_phase: float = 0.0, sweep_strength: float = 0.0) -> ImageTk.PhotoImage:
        with _PIL_DRAW_LOCK:
            return self._render_top_locked(username, width, height,
                                            sweep_phase, sweep_strength)

    def _render_top_locked(self, username: str, width: int, height: int,
                           sweep_phase: float, sweep_strength: float) -> ImageTk.PhotoImage:
        width = max(1, int(width))
        height = max(1, int(height))
        # v2.2.26: quantize sweep params so steady frames + several frames
        # near the apex of the sin pulse hit the cache. Without this, even
        # static (sweep_strength=0) ticks rebuilt the entire plate.
        sp_q, ss_q = _CY_UI.menu_plate_sweep_quantized(
            sweep_phase, sweep_strength, 1.0)
        sig = (username, width, height, sp_q, ss_q)
        if sig == self._top_sig and self._top_photo is not None:
            return self._top_photo

        image = self._compose_top_image(username, width, height, sp_q, ss_q)
        self._top_sig = sig
        return self._to_photo(image, slot='top')

    def render_top_pil(self, username: str, width: int, height: int,
                       sweep_phase: float = 0.0,
                       sweep_strength: float = 0.0) -> Image.Image:
        # Worker-safe variant of :meth:`render_top` returning a raw
        # PIL Image. Does not touch Tk — safe to call from any thread.
        # Bypasses the Tk PhotoImage cache; the GPU path keeps its own
        # signature dedup.
        with _PIL_DRAW_LOCK:
            return self._render_top_pil_locked(username, width, height,
                                                sweep_phase, sweep_strength)

    def _render_top_pil_locked(self, username: str, width: int, height: int,
                               sweep_phase: float, sweep_strength: float) -> Image.Image:
        width = max(1, int(width))
        height = max(1, int(height))
        sp_q, ss_q = _CY_UI.menu_plate_sweep_quantized(
            sweep_phase, sweep_strength, 1.0)
        sig = (username, width, height, sp_q, ss_q)
        if sig == self._top_pil_sig and self._top_pil_frame is not None:
            return self._top_pil_frame
        image = self._compose_top_image(username, width, height, sp_q, ss_q)
        self._top_pil_sig = sig
        self._top_pil_frame = image
        return image

    def _compose_top_image(self, username: str, width: int, height: int,
                           sp_q: float, ss_q: float) -> Image.Image:
        body = self._get_top_body(username, width, height)
        if ss_q <= 0.005:
            return body
        image = body.copy()
        self._apply_sweep(
            image, width, height,
            sweep_phase=sp_q,
            sweep_strength=ss_q,
            tint=(220, 246, 255),
            alpha_scale=48,
            blur_radius=max(5, width * 0.030),
            slant=0.34,
        )
        return image

    def _get_top_body(self, username: str, width: int, height: int) -> Image.Image:
        key = (username, width, height)
        cached = self._top_body_cache.get(key)
        if cached is not None:
            return cached
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
        # Cap cache: open/sync animations sweep through ~16 distinct sizes;
        # bound at 64 to defend against unexpected size storms.
        if len(self._top_body_cache) > 64:
            self._top_body_cache.clear()
        self._top_body_cache[key] = image
        return image

    @_probe.decorate('ui.menu.plate_bottom')
    def render_bottom(self, description: str, width: int, height: int,
                      sweep_phase: float = 0.0, sweep_strength: float = 0.0) -> ImageTk.PhotoImage:
        with _PIL_DRAW_LOCK:
            return self._render_bottom_locked(description, width, height,
                                               sweep_phase, sweep_strength)

    def _render_bottom_locked(self, description: str, width: int, height: int,
                              sweep_phase: float, sweep_strength: float) -> ImageTk.PhotoImage:
        width = max(1, int(width))
        height = max(1, int(height))
        sp_q, ss_q = _CY_UI.menu_plate_sweep_quantized(
            sweep_phase, sweep_strength, 0.82)
        sig = (description, width, height, sp_q, ss_q)
        if sig == self._bottom_sig and self._bottom_photo is not None:
            return self._bottom_photo

        image = self._compose_bottom_image(description, width, height, sp_q, ss_q)
        self._bottom_sig = sig
        return self._to_photo(image, slot='bottom')

    def render_bottom_pil(self, description: str, width: int, height: int,
                          sweep_phase: float = 0.0,
                          sweep_strength: float = 0.0) -> Image.Image:
        # Worker-safe variant of :meth:`render_bottom`. See
        # :meth:`render_top_pil`.
        with _PIL_DRAW_LOCK:
            return self._render_bottom_pil_locked(description, width, height,
                                                   sweep_phase, sweep_strength)

    def _render_bottom_pil_locked(self, description: str, width: int, height: int,
                                  sweep_phase: float, sweep_strength: float) -> Image.Image:
        width = max(1, int(width))
        height = max(1, int(height))
        sp_q, ss_q = _CY_UI.menu_plate_sweep_quantized(
            sweep_phase, sweep_strength, 0.82)
        sig = (description, width, height, sp_q, ss_q)
        if sig == self._bottom_pil_sig and self._bottom_pil_frame is not None:
            return self._bottom_pil_frame
        image = self._compose_bottom_image(description, width, height, sp_q, ss_q)
        self._bottom_pil_sig = sig
        self._bottom_pil_frame = image
        return image

    def _compose_bottom_image(self, description: str, width: int, height: int,
                              sp_q: float, ss_q: float) -> Image.Image:
        body = self._get_bottom_body(description, width, height)
        if ss_q <= 0.005:
            return body
        image = body.copy()
        self._apply_sweep(
            image, width, height,
            sweep_phase=sp_q,
            sweep_strength=ss_q,
            tint=(255, 236, 196),
            alpha_scale=32,
            blur_radius=max(4, width * 0.024),
            slant=0.26,
        )
        return image

    def _get_bottom_body(self, description: str, width: int, height: int) -> Image.Image:
        key = (description, width, height)
        cached = self._bottom_body_cache.get(key)
        if cached is not None:
            return cached
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
        if len(self._bottom_body_cache) > 64:
            self._bottom_body_cache.clear()
        self._bottom_body_cache[key] = image
        return image

    def _apply_sweep(self, image: Image.Image, width: int, height: int,
                     sweep_phase: float, sweep_strength: float,
                     tint: Tuple[int, int, int], alpha_scale: int,
                     blur_radius: float, slant: float) -> None:
        sweep_strength = max(0.0, min(1.0, float(sweep_strength)))
        if sweep_strength <= 0.004 or width < 18 or height < 12:
            return
        sweep_phase = max(0.0, min(1.0, float(sweep_phase)))
        center = int((-0.24 + 1.30 * sweep_phase) * width)
        half_w = max(14, int(width * 0.16))
        skew = int(max(4, height * slant))
        alpha = max(6, int(alpha_scale * sweep_strength))
        overlay = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        draw = ImageDraw.Draw(overlay)
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
