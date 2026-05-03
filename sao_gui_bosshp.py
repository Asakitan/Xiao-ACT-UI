# -*- coding: utf-8 -*-
"""
sao_gui_bosshp.py — SAO Boss HP Bar overlay (tkinter + ULW)

Pixel-level port of `web/boss_hp.html` to tkinter. Drawn via PIL onto a
layered window, animated at 60 FPS:

  * Olive cover with cyan / gold corner brackets and scan-lines
  * Slanted "name plate" with diagonal cuts (clip-path approximation)
  * HP bar with skewed leading edge and lagging trail (~280 ms latency)
  * Colour-ramp fill (green → yellow → red)
  * Shield overlay with moving light sweep, fracture burst on break
  * Damage flash, break burst particles, overdrive glow
  * Break / extinction sub-bar
  * Enter / exit animations (cubic-bezier slide+fade)

Public API (kept backward-compatible with sao_gui.py):
    BossHpOverlay(root, settings=None)
    .show() / .hide() / .destroy()
    .update(data)

`data` dict keys consumed (all optional unless noted):
    active, boss_name,
    hp_pct, current_hp, total_hp, hp_source,
    shield_active, shield_pct,
    breaking_stage, extinction_pct,
    in_overdrive, invincible,
    stage_text
"""

from __future__ import annotations

import os
import sys
import time
import ctypes
import math
import threading
from types import SimpleNamespace
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import tkinter as tk
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from gpu_renderer import gaussian_blur_rgba as _gpu_blur
from overlay_scheduler import get_scheduler as _get_scheduler
from overlay_render_worker import (
    AsyncFrameWorker, clip_alpha_image, multiply_alpha_image,
    submit_ulw_commit,
)
from overlay_subpixel import subpixel_bar_width

# v2.3.x: optional GPU presenter. Env-gated via SAO_GPU_BOSSHP
# (defaults to SAO_GPU_OVERLAY). Falls back to ULW if GLFW is unavailable.
# BossHP is intentionally fixed-position and click-through.
try:
    import gpu_overlay_window as _gow  # type: ignore[import-untyped]
except Exception:
    _gow = None  # type: ignore[assignment]


def _gpu_bosshp_enabled() -> bool:
    if _gow is None or not _gow.glfw_supported():
        return False
    try:
        import os as _os
        flag = _os.environ.get('SAO_GPU_BOSSHP')
        if flag is None:
            return True
        return str(flag).strip() not in ('', '0', 'false', 'False')
    except Exception:
        return False

from perf_probe import gauge as _perf_gauge, probe as _probe

# Reuse ULW glue + font helpers from sao_gui_dps so we keep the same
# premultiply path and font cache.
from sao_gui_dps import (  # noqa: F401
    _ulw_update, _user32, _load_font, _pick_font, _text_width,
    _has_cjk, _ease_out_cubic, _lerp,
    GWL_EXSTYLE, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_EX_TRANSPARENT,
)


# ═══════════════════════════════════════════════
#  Formatting
# ═══════════════════════════════════════════════

def _fmt_hp(v: float) -> str:
    v = float(v or 0)
    if v >= 1_000_000_000:
        return f'{v / 1_000_000_000:.2f}B'
    if v >= 1_000_000:
        return f'{v / 1_000_000:.2f}M'
    if v >= 10_000:
        return f'{v / 1_000:.1f}K'
    return f'{int(round(v)):,}'


def _mix(a: Tuple[int, int, int, int],
         b: Tuple[int, int, int, int], t: float) -> Tuple[int, int, int, int]:
    t = max(0.0, min(1.0, t))
    return (
        int(_lerp(a[0], b[0], t)),
        int(_lerp(a[1], b[1], t)),
        int(_lerp(a[2], b[2], t)),
        int(_lerp(a[3], b[3], t)),
    )


def _clip_alpha(img: Image.Image, mask: Image.Image) -> Image.Image:
    """Return `img` with its alpha multiplied by `mask` (L-mode). Used to
    clip arbitrary layers to the rounded-rect panel interior."""
    return clip_alpha_image(img, mask)


def _draw_text_shadow(img: Image.Image, xy, text: str, font,
                      shadow_color: Tuple[int, int, int, int],
                      blur: int = 3) -> None:
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).text(xy, text, fill=shadow_color, font=font)
    img.alpha_composite(_gpu_blur(layer, blur))


def _offset_poly(points, dx: int, dy: int):
    return [(x + dx, y + dy) for x, y in points]


def _apply_inset_shadow(img: Image.Image, mask: Image.Image,
                        color: Tuple[int, int, int],
                        alpha: int, blur_radius: float) -> None:
    """Composite a CSS-style `inset 0 0 Npx rgba(color, alpha)` glow
    onto `img`, clipped by `mask`. The glow is bright at the rim of the
    shape and fades inward, exactly like a CSS inset box-shadow.

    Implementation: blur the *inverted* mask (so brightness leaks from
    outside into the shape) and clip back to the original mask.

    v2.2.11 Phase 3: prefer the fused GPU path
    (`_apply_inset_shadow_gpu`) when the per-thread compositor is
    available. Falls back to the CPU/PIL path on any failure so older
    GPUs / RDP sessions keep working.
    """
    if _apply_inset_shadow_gpu(img, mask, color, alpha, blur_radius):
        return
    # CPU fallback (original implementation): invert mask → write into RGBA
    # alpha channel → GPU blur → clip back to the host mask.
    inv = Image.eval(mask, lambda v: 255 - v)
    rgba = np.zeros((img.size[1], img.size[0], 4), dtype=np.uint8)
    rgba[:, :, 0] = color[0]
    rgba[:, :, 1] = color[1]
    rgba[:, :, 2] = color[2]
    rgba[:, :, 3] = np.asarray(inv, dtype=np.uint8)
    blurred = _gpu_blur(Image.fromarray(rgba, 'RGBA'), blur_radius)
    glow_alpha = np.asarray(blurred, dtype=np.uint16)[:, :, 3]
    glow_alpha = (glow_alpha * alpha // 255).astype(np.uint8)
    out = np.zeros((img.size[1], img.size[0], 4), dtype=np.uint8)
    out[:, :, 0] = color[0]
    out[:, :, 1] = color[1]
    out[:, :, 2] = color[2]
    out[:, :, 3] = glow_alpha
    glow_img = Image.fromarray(out, 'RGBA')
    img.alpha_composite(_clip_alpha(glow_img, mask))


def _apply_inset_shadow_gpu(img: Image.Image, mask: Image.Image,
                            color: Tuple[int, int, int],
                            alpha: int, blur_radius: float) -> bool:
    """Single-pass GPU implementation of the inset shadow.

    Returns True on success, False to let the caller use the CPU path.

    v2.2.11 Phase 3 refinement: the entire pipeline now stays on the
    GPU — invert mask → upload → ``blur_tex`` (compositor-resident
    two-pass separable) → upload shape mask → ``inset_shadow`` shader
    → download → composite.  Removes 2 PIL↔GPU roundtrips compared to
    the initial implementation.  Net win: ~0.8–1.5 ms per BossHP frame
    over the original CPU path.
    """
    try:
        from gpu_compositor import LayerCompositor  # noqa: F401
    except Exception:
        return False
    try:
        comp = _get_thread_compositor()
        if comp is None or not comp.available:
            return False
        w, h = img.size
        mask_arr = np.asarray(mask, dtype=np.uint8)
        if mask_arr.shape != (h, w):
            return False
        # 1) Build inverted-mask RGBA (cheap numpy) and upload to GPU.
        inv_rgba = np.empty((h, w, 4), dtype=np.uint8)
        inv_rgba[:, :, 0] = color[0]
        inv_rgba[:, :, 1] = color[1]
        inv_rgba[:, :, 2] = color[2]
        inv_rgba[:, :, 3] = 255 - mask_arr
        inv_tex = comp.upload('__inset_inv', inv_rgba)
        if inv_tex is None:
            return False
        # 2) GPU-resident two-pass blur (no PIL roundtrip).
        blurred_tex = comp.blur_tex(inv_tex, blur_radius,
                                    out_tag='__inset_blur')
        if blurred_tex is None:
            return False
        # 3) Upload shape mask + run fused inset_shadow shader.
        shape_rgba = np.empty((h, w, 4), dtype=np.uint8)
        shape_rgba[:, :, 0] = color[0]
        shape_rgba[:, :, 1] = color[1]
        shape_rgba[:, :, 2] = color[2]
        shape_rgba[:, :, 3] = mask_arr
        shape_tex = comp.upload('__inset_shape', shape_rgba)
        out_tex = comp.tex('__inset_out', w, h, clear=True)
        if shape_tex is None or out_tex is None:
            return False
        comp.render(
            'inset_shadow', out_tex,
            uniforms={
                'u_color': (color[0] / 255.0, color[1] / 255.0,
                            color[2] / 255.0, 1.0),
                'u_intensity': max(0.0, min(1.0, alpha / 255.0)),
            },
            inputs={'u_blurred_inv': blurred_tex,
                    'u_shape': shape_tex},
        )
        glow = comp.to_pil(out_tex)
        if glow is None:
            return False
        img.alpha_composite(glow)
        return True
    except Exception:
        return False


# Per-thread compositor handle for BossHP (re-used for any GPU-fused
# helper that runs on the panel's render-lane thread).
_inset_tls = threading.local()


def _get_thread_compositor():
    comp = getattr(_inset_tls, 'compositor', None)
    if comp is not None:
        return comp
    try:
        from gpu_compositor import LayerCompositor
        comp = LayerCompositor('bosshp')
    except Exception:
        comp = None
    _inset_tls.compositor = comp
    return comp


# ═══════════════════════════════════════════════
#  Overlay
# ═══════════════════════════════════════════════

class BossHpOverlay:
    """Animated SAO-styled boss HP overlay (ULW + PIL)."""

    # Canvas size: 560×88 root panel plus the same outer FX bleed used by
    # web/boss_hp.html (`break-burst-layer` is 620×108 at -30,-10).
    WIDTH = 620
    HEIGHT = 184

    # Panel (the olive cover)
    PANEL_X = 30
    PANEL_Y = 14
    PANEL_W = 560
    PANEL_H = 88

    # Boss-box (content inside the cover)
    BOX_X = PANEL_X + 16
    BOX_Y = PANEL_Y + 12
    BOX_W = 492
    BOX_H = 45

    NUMBER_X = BOX_X + int(round(BOX_W * 0.58))
    NUMBER_Y = BOX_Y + int(round(BOX_H * 0.82))
    NUMBER_W = 210
    NUMBER_H = 19

    # HP bar geometry (relative to panel origin, matches CSS
    # xt_border left:108, top:9 inside boss-box which is at 16,12).
    BAR_X = PANEL_X + 16 + 108        # 134
    BAR_Y = PANEL_Y + 12 + 9          # 25
    BAR_W = 342
    BAR_H = 23

    # Break row (inside the root panel, like the webview).
    BREAK_LABEL_X = PANEL_X + 22
    BREAK_ROW_Y = PANEL_Y + 64
    BREAK_TRACK_X = PANEL_X + 56
    BREAK_TRACK_W = 404
    BREAK_TRACK_H = 6
    BREAK_TEXT_X = BREAK_TRACK_X + BREAK_TRACK_W + 7

    # Secondary unit mini panels (mirrors web/boss_hp.html additional-units).
    ADD_Y = PANEL_Y + PANEL_H + 12
    ADD_W = 136
    ADD_H = 40
    ADD_GAP = 8

    # ── Palette (matches CSS :root) ────────────────────────────────
    # v2.2.0: SAO Alert flat hi-tech — 纯白+略灰 (alpha 沿用)
    COVER_A = (244, 246, 247, 255)
    COVER_MID = (232, 235, 238, 255)
    COVER_B = (223, 227, 231, 255)
    COVER_EDGE = (186, 190, 196, 255)
    COVER_EDGE_DEEP = (160, 165, 171, 255)
    LINE = (214, 216, 219, 255)
    LINE_SOFT = (246, 247, 248, 255)
    HAIRLINE_LIGHT = (248, 249, 250, 255)
    HAIRLINE_MID = (226, 229, 232, 255)
    HAIRLINE_DARK = (160, 165, 171, 255)
    TEXT_SHADOW = (190, 192, 195, 255)
    TEXT_MAIN = (100, 99, 100, 255)
    TEXT_MUTED = (140, 135, 138, 255)
    CYAN = (104, 228, 255, 255)
    GOLD_CORNER = (212, 156, 23, 255)
    GOLD_STRONG = (222, 166, 32, 255)
    RED = (239, 104, 78, 255)
    BOX_BG = (248, 249, 250, 255)
    BAR_TRACK = (172, 176, 182, 42)
    TRAIL_COLOR = (255, 255, 255, 58)

    # HP gradients (left → right) — fully opaque, matching CSS rgb()
    HP_GREEN = ((211, 234, 124, 255), (154, 211, 52, 255))
    HP_YELLOW = ((235, 238, 112, 255), (244, 250, 73, 255))
    HP_RED = ((248, 140, 122, 255), (239, 104, 78, 255))

    # Shield colours
    SHIELD_A = (72, 156, 232, 107)
    SHIELD_B = (98, 208, 255, 133)
    SHIELD_C = (212, 248, 255, 61)

    # Animation tuning (seconds)
    HP_TWEEN = 0.32
    TRAIL_TWEEN = 0.82
    TRAIL_LAG = 0.28            # delay trail catch-up on HP drop
    SHIELD_TWEEN = 0.28
    BREAK_TWEEN = 0.24
    FADE_IN = 0.32
    FADE_OUT = 0.26
    DAMAGE_FLASH = 0.45
    BREAK_BURST_FX_S = 1.4     # webview _breakBurstFxMs
    BREAK_BURST_PHASE1_S = 0.50
    SHIELD_BREAK = 2.5
    SHIELD_VFX_BREAK_S = 1.4
    SHIELD_VFX_RESTORE_S = 1.04
    BREAK_VFX_BREAK_S = 1.4
    BREAK_VFX_RECOVER_S = 1.32
    BREAK_VFX_RESTORED_S = 0.98
    ROOT_PULSE_SHIELD_BREAK_S = 2.5
    ROOT_PULSE_SHIELD_RESTORE_S = 0.86
    ROOT_PULSE_BREAK_HIT_S = 2.5
    ROOT_PULSE_BREAK_RECOVER_S = 0.96

    # Break state machine timing (matching webview)
    BREAK_HOLD_S = 2.5          # hold at 0% before recovering
    RECOVER_PHASE1_S = 5.0      # filling 0→85% (ease-out quadratic)
    RECOVER_CAP_PCT = 0.85
    RECOVER_CAP_HOLD_S = 0.30   # hold at 85%
    RECOVER_SLOWTAIL_S = 8.0    # 85%→98% (linear)
    RECOVER_SLOWTAIL_PCT = 0.98
    RECOVER_FASTFILL_S = 0.42   # current→100% (cubic ease-out)
    REFILLED_HOLD_S = 0.52      # hold at 100% then → normal
    POST_REFILL_COOLDOWN_S = 4.0
    TCP_COOLDOWN_S = 2.5

    TICK_MS = 16          # damping coefficient base; not scheduling rate (overlay_scheduler owns Hz)
    IDLE_TICK_MS = 50

    def __init__(self, root: tk.Tk, settings: Any = None):
        self.root = root
        self.settings = settings
        self._win: Optional[tk.Toplevel] = None
        # v2.3.x GPU presenter fields.
        self._gpu_window: Optional[Any] = None
        self._gpu_presenter: Optional[Any] = None
        self._gpu_managed: bool = False
        self._hwnd: int = 0
        self._visible = False
        self._destroying = False
        self._last_data: Optional[dict] = None

        self._x, self._y = self._fixed_position()

        # Animated state
        self._disp_hp_pct = 1.0
        self._target_hp_pct = 1.0
        self._disp_trail_pct = 1.0
        self._target_trail_pct = 1.0
        self._trail_pending_time = 0.0  # when to start moving the trail

        self._disp_shield_pct = 0.0
        self._target_shield_pct = 0.0
        self._shield_active = False
        self._last_shield_active = False
        self._last_shield_pct = 0.0

        self._disp_break_pct = 1.0
        self._target_break_pct = 1.0
        self._breaking_stage = 0
        self._last_breaking_stage = -1
        # Webview-parity break state machine (4 states)
        self._break_state = 'normal'    # 'normal' | 'broken' | 'recovering' | 'refilled'
        self._has_break_data = False
        self._stop_breaking_ticking = False
        self._last_stop_breaking_ticking = False
        self._first_break_seen = False
        self._last_break_pct = 1.0
        self._break_ever_had_gauge = False

        # Break timing state
        self._break_entered_ts = 0.0
        self._break_hold_timer = 0.0    # when to transition broken→recovering
        self._refill_completed_ts = 0.0
        self._refills_completed = 0
        self._tcp_break_triggered = False
        self._tcp_break_ts = 0.0
        self._break_stale_ts = 0.0      # when stale detection started
        self._last_hp_changed_ts = 0.0
        self._prev_hp_for_break = -1.0

        # Recovering sub-phases: 'idle'|'filling'|'capped'|'slowtail'|'fastfill'
        self._recover_phase = 'idle'
        self._recover_start_ts = 0.0
        self._recover_current_pct = 0.0
        self._recover_cap_timer = 0.0   # when capped phase ends
        self._slowtail_start_ts = 0.0
        self._slowtail_start_pct = 0.0
        self._fastfill_start_ts = 0.0
        self._fastfill_start_pct = 0.0
        self._recover_raw_peak_pct = 0.0
        self._recover_interpolating = False

        self._in_overdrive = False
        self._invincible = False
        self._boss_name = 'Enemy'
        self._stage_text = ''
        self._current_hp = 0.0
        self._total_hp = 0.0
        self._hp_source = ''
        self._additional_units: List[Dict[str, Any]] = []

        # FX state
        self._damage_flash_start = 0.0
        self._break_burst_start = 0.0
        self._shield_break_start = 0.0
        self._shield_ghost_pct = 0.0
        self._shield_light_phase = 0.0  # accumulates for light sweep
        self._last_render_t = 0.0
        self._flash_type = ''           # active flash overlay type
        self._flash_start = 0.0
        self._shield_vfx_mode = ''
        self._shield_vfx_start = 0.0
        self._shield_vfx_duration = 0.0
        self._break_vfx_mode = ''
        self._break_vfx_start = 0.0
        self._break_vfx_duration = 0.0
        self._root_pulse_mode = ''
        self._root_pulse_start = 0.0
        self._root_pulse_duration = 0.0

        # Entry / exit
        self._fade_alpha = 0.0
        self._fade_target = 0.0
        self._fade_from = 0.0
        self._fade_start = 0.0
        self._fade_duration = self.FADE_IN
        self._enter_translate = 10.0  # px, animates 10→0 on enter
        self._exiting = False
        self._hide_after_exit = False

        self._tick_after_id: Optional[str] = None
        self._registered: bool = False
        self._drag_ox = 0
        self._drag_oy = 0
        self._gpu_drag_active = False

        # Cached panel layers (expensive to redraw every frame).
        self._cache_cover: Optional[Image.Image] = None
        self._cache_cover_mask: Optional[Image.Image] = None
        self._cache_bar_mask: Optional[Image.Image] = None
        self._cache_panel_mask: Optional[Image.Image] = None
        # Composited static base (cover + corners + boss box + bar track).
        self._static_cache: Optional[Image.Image] = None
        self._static_y_off: int = -9999
        # v2.2.18 (Phase 3a): per-frame text layer cache. Name plate +
        # HP digits + source tag are content-driven (no animation), but
        # were redrawn every frame including a _gpu_blur(2) per shadow
        # call. Cache by (y_off, boss_name, hp_source, current_hp,
        # total_hp, disp_hp_pct quantized, invincible).
        self._text_layer_cache: Optional[Image.Image] = None
        self._text_layer_sig: tuple = ()
        # v2.3.0 Phase 1: full-frame cache. compose_frame is called at
        # 60 Hz on the BOSSHP render lane and most consecutive frames
        # only differ by quantizable animation phase. When the
        # signature matches the previous compose, return the same
        # Image instance — the worker only reads it (premultiply path).
        self._frame_cache: Optional[Image.Image] = None
        self._frame_sig: tuple = ()
        self._frame_version: int = 0

        # Async render worker — compose + premult off main thread.
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)

        # v2.2.14: idle short-circuit. Once a steady frame has been
        # composed and committed (no animation in flight) we skip both
        # compose and submit until ``_is_animating()`` flips back to
        # True. Drives idle CPU back toward webview parity.
        self._idle_committed = False

        # Theme: load saved preference
        self._theme_name: str = 'dark'
        if settings is not None:
            try:
                saved = settings.get('panel_themes', {}).get('bosshp', 'dark')
                if saved in ('light', 'dark'):
                    self._apply_theme(saved)
            except Exception:
                pass

    # ── Theme ──

    @classmethod
    def _fixed_position(cls) -> Tuple[int, int]:
        # Center the visible 560px cover while keeping the wider FX canvas.
        sw = _user32.GetSystemMetrics(0)
        return (int((sw - cls.PANEL_W) // 2 - cls.PANEL_X), 12)

    def _restore_fixed_position(self) -> None:
        self._x, self._y = self._fixed_position()

    def _apply_theme(self, theme_name: str) -> None:
        """切换 BossHP 面板主题并清除所有渲染缓存。"""
        from sao_theme import get_panel_theme
        theme = get_panel_theme('bosshp', theme_name)
        if not theme:
            return
        for key, value in theme.items():
            setattr(self, key, value)
        self._theme_name = theme_name
        self._cache_cover = None
        self._cache_cover_mask = None
        self._cache_bar_mask = None
        self._cache_panel_mask = None
        self._static_cache = None; self._static_y_off = -9999
        self._text_layer_cache = None; self._text_layer_sig = ()
        self._frame_cache = None; self._frame_sig = ()
        self._frame_version += 1
        # Break idle-commit guard so the next _tick submits a frame.
        self._idle_committed = False
        self._schedule_tick(immediate=True)

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def show(self) -> None:
        if self._win is not None:
            return
        self._restore_fixed_position()
        # v2.3.x: GPU presenter path (env-gated).
        if _gpu_bosshp_enabled():
            try:
                pump = _gow.get_glfw_pump(self.root)
                presenter = _gow.BgraPresenter()
                gpu_win = _gow.GpuOverlayWindow(
                    pump,
                    w=int(self.WIDTH), h=int(self.HEIGHT),
                    x=int(self._x), y=int(self._y),
                    render_fn=presenter.render,
                    click_through=True,
                    title='sao_bosshp_gpu',
                )
                gpu_win.show()
                self._gpu_window = gpu_win
                self._gpu_presenter = presenter
                self._gpu_managed = True
                self._win = self  # type: ignore[assignment]  # sentinel
                self._hwnd = 0
                self._visible = True
                self._destroying = False
                self._fade_from = 0.0
                self._fade_alpha = 0.0
                self._fade_target = 1.0
                self._fade_start = time.time()
                self._fade_duration = self.FADE_IN
                self._enter_translate = 10.0
                self._exiting = False
                self._hide_after_exit = False
                self._schedule_tick(immediate=True)
                return
            except Exception:
                self._gpu_window = None
                self._gpu_presenter = None
                self._gpu_managed = False
        self._win = tk.Toplevel(self.root)
        # Black bg + 1x1 initial geometry prevents any default-bg white flash
        # before the first UpdateLayeredWindow call commits real pixels.
        try:
            self._win.configure(bg='black')
        except Exception:
            pass
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(f'1x1+{self._x}+{self._y}')
        self._win.update_idletasks()

        try:
            self._hwnd = _user32.GetParent(self._win.winfo_id()) or \
                self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()

        ex = _user32.GetWindowLongW(ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST
            | WS_EX_TRANSPARENT,
        )
        # 防御性清理：移除可能被 _apply_panel_style() 设置的 CS_DROPSHADOW
        try:
            _GCL_STYLE, _CS_DS = -26, 0x00020000
            _cls = ctypes.windll.user32.GetClassLongW(self._hwnd, _GCL_STYLE)
            if _cls & _CS_DS:
                ctypes.windll.user32.SetClassLongW(
                    self._hwnd, _GCL_STYLE, _cls & ~_CS_DS)
        except Exception:
            pass
        try:
            _user32.SetWindowDisplayAffinity(ctypes.c_void_p(self._hwnd), 0x00000011)
        except Exception:
            pass
        # Disable DWM non-client rendering (incl. system drop shadow) so the
        # shadow does not linger while the ULW bitmap fades to transparent.
        try:
            _ncr_disabled = ctypes.c_int(1)   # DWMNCRP_DISABLED
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                ctypes.c_void_p(self._hwnd), 2,
                ctypes.byref(_ncr_disabled), ctypes.sizeof(_ncr_disabled))
        except Exception:
            pass

        self._visible = True
        self._destroying = False
        # Entry animation
        self._fade_from = 0.0
        self._fade_alpha = 0.0
        self._fade_target = 1.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_IN
        self._enter_translate = 10.0
        self._exiting = False
        self._hide_after_exit = False
        self._schedule_tick(immediate=True)

    def hide(self) -> None:
        # Play the exit animation then destroy on completion.
        if self._win is None:
            return
        if self._exiting:
            return
        self._exiting = True
        self._fade_from = self._fade_alpha
        self._fade_target = 0.0
        self._fade_start = time.time()
        self._fade_duration = self.FADE_OUT
        self._hide_after_exit = True
        self._schedule_tick(immediate=True)

    def destroy(self) -> None:
        self._cancel_tick()
        if hasattr(self, '_render_worker') and self._render_worker is not None:
            try:
                self._render_worker.stop()
            except Exception:
                pass
        # GPU presenter teardown first (mirrors SkillFX/HP pattern).
        if self._gpu_presenter is not None:
            try:
                self._gpu_presenter.release()
            except Exception:
                pass
            self._gpu_presenter = None
        if self._gpu_window is not None:
            try:
                self._gpu_window.destroy()
            except Exception:
                pass
            self._gpu_window = None
        if self._win is not None and self._win is not self:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._gpu_managed = False
        self._gpu_drag_active = False
        self._visible = False
        self._exiting = False
        self._hide_after_exit = False

    def _window_ready(self) -> bool:
        return bool(self._gpu_managed or self._hwnd)

    @_probe.decorate('ui.bosshp.update')
    def update(self, data: dict) -> None:
        """Ingest a snapshot; drive the animation loop."""
        if data is None:
            return
        if not data.get('active', False):
            self._additional_units = []
            # Boss inactive → animate out.
            if self._visible and not self._exiting:
                self.hide()
            elif not self._visible:
                # Already hidden, nothing to do.
                pass
            return

        # Cancel any pending exit — boss re-appeared.
        if self._exiting:
            self._exiting = False
            self._hide_after_exit = False
            self._fade_from = self._fade_alpha
            self._fade_target = 1.0
            self._fade_start = time.time()
            self._fade_duration = self.FADE_IN

        if not self._visible or not self._window_ready():
            self.show()

        self._last_data = data
        self._idle_committed = False

        # ── targets ──
        name = str(data.get('boss_name') or 'Enemy').strip() or 'Enemy'
        self._boss_name = name
        self._stage_text = str(data.get('stage_text') or '')

        hp_pct = max(0.0, min(1.0, float(data.get('hp_pct') or 0)))
        if hp_pct < self._target_hp_pct - 0.001:
            # Damage taken → schedule trail catch-up after a short lag.
            # Per webview reference, damage flash is NOT triggered on generic
            # HP decrease — only on break/shield events via _trigger_flash.
            self._trail_pending_time = time.time() + self.TRAIL_LAG
            self._target_trail_pct_pending = hp_pct
        else:
            # Heal or first frame → snap trail forward with the fill.
            self._target_trail_pct = hp_pct
            self._trail_pending_time = 0.0
        self._target_hp_pct = hp_pct

        self._current_hp = float(data.get('current_hp') or 0)
        self._total_hp = float(data.get('total_hp') or 0)
        self._hp_source = str(data.get('hp_source') or '')
        self._additional_units = self._normalize_additional_units(
            data.get('additional') or [])

        shield_active = bool(data.get('shield_active', False))
        shield_pct = max(0.0, min(1.0, float(data.get('shield_pct') or 0)))

        # ── Shield liveness heuristic ──
        # If shield_active is True but shield_pct is 0 and the boss HP has been
        # decreasing, the server likely stopped sending ShieldList updates after
        # the shield was depleted. Force shield_active = False so the overlay
        # correctly hides the shield bar.
        if shield_active and shield_pct < 0.001:
            _now_sh = time.time()
            if not hasattr(self, '_shield_zero_ts'):
                self._shield_zero_ts: float = 0.0
            if self._prev_hp_for_break > 0 and hp_pct < self._prev_hp_for_break - 0.001:
                # Boss HP is actively decreasing while shield reports 0% —
                # the shield was likely consumed. If this has been the case
                # for more than 0.8s, force-clear the active flag.
                if self._shield_zero_ts == 0.0:
                    self._shield_zero_ts = _now_sh
                elif _now_sh - self._shield_zero_ts > 0.8:
                    shield_active = False
            else:
                # Reset timer when HP is stable or increasing
                self._shield_zero_ts = 0.0

        if self._last_shield_active and not shield_active:
            # Shield just broke → retain a ghost shell and fire the full
            # shield break FX stack (cover pulse + outer bloom + bar flash).
            self._shield_ghost_pct = max(self._last_shield_pct, 0.08)
            self._trigger_shield_fx('breaking')
            # If boss is in broken state, skip hold and start recovery.
            if self._break_state == 'broken':
                self._begin_recovery()
                self._trigger_recover_fastfill()
            elif self._break_state == 'recovering':
                self._trigger_recover_fastfill()
        elif not self._last_shield_active and shield_active:
            self._trigger_shield_fx('restoring')
        self._last_shield_active = shield_active
        self._last_shield_pct = shield_pct
        self._shield_active = shield_active
        self._target_shield_pct = shield_pct if shield_active else 0.0

        stage = int(data.get('breaking_stage') if data.get('breaking_stage') is not None else -1)
        ext_pct = max(0.0, min(1.0, float(data.get('extinction_pct') or 0)))
        has_break = bool(data.get('has_break_data', False))
        stop_ticking = bool(data.get('stop_breaking_ticking', False))

        # Track HP changes for stale break detection.
        if abs(hp_pct - self._prev_hp_for_break) > 0.005:
            self._last_hp_changed_ts = time.time()
            self._prev_hp_for_break = hp_pct

        # Track if break gauge has ever been significantly filled.
        if ext_pct > 0.50:
            self._break_ever_had_gauge = True

        self._breaking_stage = stage
        self._has_break_data = has_break
        self._stop_breaking_ticking = stop_ticking

        # Visual locking: don't move the break bar while the state machine
        # is controlling it (broken, recovering, refilled, or post-refill cooldown).
        post_refill_lock = (self._refill_completed_ts > 0
                            and time.time() - self._refill_completed_ts < 2.0
                            and self._refills_completed >= 2)
        visual_locked = (self._break_state in ('broken', 'refilled')
                         or self._recover_interpolating
                         or post_refill_lock)

        if not visual_locked and has_break:
            self._target_break_pct = ext_pct
        elif self._break_state == 'broken':
            self._target_break_pct = 0.0

        # Run break state machine.
        self._update_break_state(ext_pct, stage, stop_ticking)

        self._last_breaking_stage = stage
        self._last_stop_breaking_ticking = stop_ticking
        if stage >= 0:
            self._first_break_seen = True
        self._last_break_pct = ext_pct

        self._in_overdrive = bool(data.get('in_overdrive', False))
        self._invincible = bool(data.get('invincible', False))

        self._schedule_tick(immediate=True)

    def _normalize_additional_units(self, units: Any) -> List[Dict[str, Any]]:
        normalized: List[Dict[str, Any]] = []
        if not isinstance(units, list):
            return normalized
        for raw in units[:4]:
            if not isinstance(raw, dict):
                continue
            try:
                hp_pct = max(0.0, min(1.0, float(raw.get('hp_pct') or 0.0)))
            except Exception:
                hp_pct = 0.0
            try:
                ext_pct = max(0.0, min(1.0, float(raw.get('extinction_pct') or 0.0)))
            except Exception:
                ext_pct = 0.0
            try:
                shield_pct = max(0.0, min(1.0, float(raw.get('shield_pct') or 0.0)))
            except Exception:
                shield_pct = 0.0
            normalized.append({
                'name': str(raw.get('name') or 'Unit')[:20],
                'hp_pct': hp_pct,
                'extinction_pct': ext_pct,
                'has_break_data': bool(raw.get('has_break_data', False)),
                'breaking_stage': int(raw.get('breaking_stage') or -1),
                'shield_active': bool(raw.get('shield_active', False)),
                'shield_pct': shield_pct,
            })
        return normalized

    def _trigger_root_pulse(self, mode: str, duration: float) -> None:
        self._root_pulse_mode = str(mode or '')
        self._root_pulse_start = time.time()
        self._root_pulse_duration = max(0.0, float(duration or 0.0))

    def _trigger_shield_fx(self, mode: str) -> None:
        now = time.time()
        self._shield_vfx_mode = 'breaking' if mode == 'breaking' else 'restoring'
        self._shield_vfx_start = now
        if mode == 'breaking':
            self._shield_break_start = now
            self._shield_vfx_duration = self.SHIELD_VFX_BREAK_S
            self._trigger_root_pulse('shield-break', self.ROOT_PULSE_SHIELD_BREAK_S)
            self._trigger_flash('shield-break')
        else:
            self._shield_vfx_duration = self.SHIELD_VFX_RESTORE_S
            self._trigger_root_pulse('shield-restore', self.ROOT_PULSE_SHIELD_RESTORE_S)
            self._trigger_flash('shield-restore')

    def _trigger_break_burst_fx(self) -> None:
        now = time.time()
        self._break_burst_start = now
        self._break_vfx_mode = 'breaking'
        self._break_vfx_start = now
        self._break_vfx_duration = self.BREAK_VFX_BREAK_S
        self._trigger_root_pulse('break-hit', self.ROOT_PULSE_BREAK_HIT_S)
        self._trigger_flash('break-burst')

    def _trigger_break_recovery_fx(self, mode: str) -> None:
        now = time.time()
        self._break_vfx_mode = 'restored' if mode == 'restored' else 'recovering'
        self._break_vfx_start = now
        if mode == 'restored':
            self._break_vfx_duration = self.BREAK_VFX_RESTORED_S
            self._trigger_root_pulse('break-recover', self.ROOT_PULSE_BREAK_RECOVER_S)
            self._trigger_flash('break-restore')
        else:
            self._break_vfx_duration = self.BREAK_VFX_RECOVER_S
            self._trigger_root_pulse('break-recover', self.ROOT_PULSE_BREAK_RECOVER_S)
            self._trigger_flash('phase-break')

    # ──────────────────────────────────────────
    #  Break State Machine (webview parity)
    # ──────────────────────────────────────────

    def _enter_broken(self) -> None:
        """Transition to broken state: bar forced to 0%, burst VFX fires."""
        self._break_state = 'broken'
        self._break_entered_ts = time.time()
        self._tcp_break_triggered = False
        self._refill_completed_ts = 0.0
        self._cancel_recover()
        self._break_stale_ts = 0.0

        # Force bar to 0%.
        self._target_break_pct = 0.0
        self._disp_break_pct = 0.0
        self._last_break_pct = 0.0

        self._trigger_break_burst_fx()

        # Schedule hold→recovering transition.
        self._break_hold_timer = time.time() + self.BREAK_HOLD_S

    def _begin_recovery(self) -> None:
        """Transition from broken to recovering with interpolation."""
        self._break_state = 'recovering'
        self._recover_phase = 'filling'
        self._recover_start_ts = time.time()
        self._recover_current_pct = 0.0
        self._recover_interpolating = True
        self._recover_raw_peak_pct = 0.0
        self._break_hold_timer = 0.0
        self._trigger_break_recovery_fx('recovering')

    def _trigger_recover_fastfill(self) -> None:
        """Jump to fastfill sub-phase (final 420ms cubic ease to 100%)."""
        if self._recover_phase == 'fastfill':
            return
        self._recover_cap_timer = 0.0
        self._fastfill_start_ts = time.time()
        self._fastfill_start_pct = self._recover_current_pct or 0.0
        self._recover_phase = 'fastfill'
        self._recover_interpolating = True

    def _cancel_recover(self) -> None:
        """Cancel any active recovery interpolation."""
        self._recover_interpolating = False
        self._recover_current_pct = 0.0
        self._recover_phase = 'idle'
        self._recover_raw_peak_pct = 0.0
        self._recover_cap_timer = 0.0

    def _enter_refilled(self) -> None:
        """Break bar fully recovered → hold at 100% briefly then → normal."""
        self._break_state = 'refilled'
        self._cancel_recover()
        self._target_break_pct = 1.0
        self._disp_break_pct = 1.0
        self._trigger_break_recovery_fx('restored')
        # Schedule transition back to normal.
        self._refill_completed_ts = time.time() + self.REFILLED_HOLD_S

    def _update_break_state(self, break_pct: float, stage: int,
                            stop_ticking: bool) -> None:
        """Webview-parity break state machine dispatch."""
        now = time.time()
        prev_stage = self._last_breaking_stage
        prev_stop = self._last_stop_breaking_ticking

        if break_pct > 0.50:
            self._break_ever_had_gauge = True

        if self._break_state == 'normal':
            # Guard: TCP cooldown.
            if self._tcp_break_triggered and now - self._tcp_break_ts < self.TCP_COOLDOWN_S:
                return
            # Guard: post-refill cooldown (after 2nd+ repair).
            if (self._refill_completed_ts > 0
                    and now - self._refill_completed_ts < self.POST_REFILL_COOLDOWN_S
                    and self._refills_completed >= 2):
                return

            # Trigger 1: breaking_stage transitions to 0 from ≥1 or -1.
            if (prev_stage >= 1 or prev_stage == -1) and stage != prev_stage and stage == 0:
                self._enter_broken()
                return

            # Trigger 2: stop_breaking_ticking edge False→True with low break%.
            if stop_ticking and not prev_stop and break_pct <= 0.08:
                self._enter_broken()
                return

            if not self._break_ever_had_gauge:
                return

            # Trigger 3: stale detection — break stuck ≤8% while HP dropping.
            hp_dropping = (self._last_hp_changed_ts > 0
                           and now - self._last_hp_changed_ts < 3.0)
            if hp_dropping and break_pct <= 0.08 and break_pct == self._last_break_pct and self._last_break_pct >= 0:
                if self._break_stale_ts == 0.0:
                    self._break_stale_ts = now
                elif now - self._break_stale_ts >= 0.35:
                    self._break_stale_ts = 0.0
                    self._enter_broken()
                    return
            elif break_pct <= 0.08 and self._last_break_pct <= 0.08 and self._last_break_pct >= 0:
                if self._break_stale_ts == 0.0:
                    self._break_stale_ts = now
                elif now - self._break_stale_ts >= 0.50:
                    self._break_stale_ts = 0.0
                    self._enter_broken()
                    return
            else:
                self._break_stale_ts = 0.0

            if break_pct > 0.08 and self._break_stale_ts:
                self._break_stale_ts = 0.0

        elif self._break_state == 'broken':
            broken_age = now - self._break_entered_ts

            # Early exit: stage→1 (BreakEnd packet) or raw pct very high.
            if (stage == 1 and prev_stage != 1) or (broken_age > 3.0 and break_pct >= 0.90):
                self._begin_recovery()
                self._trigger_recover_fastfill()
                return

            # Early exit: break pct rising above 15% after 2s hold.
            if broken_age >= 2.0 and break_pct > self._last_break_pct and break_pct > 0.15:
                self._begin_recovery()
                return

            # Normal hold→recovering after BREAK_HOLD_S.
            if self._break_hold_timer > 0 and now >= self._break_hold_timer:
                self._begin_recovery()
                return

        elif self._break_state == 'recovering':
            self._recover_raw_peak_pct = max(self._recover_raw_peak_pct, break_pct)

            # Fast-fill triggers.
            if stage == 1 and prev_stage != 1:
                self._trigger_recover_fastfill()
                return
            if break_pct >= 0.95 and self._recover_phase != 'fastfill':
                self._trigger_recover_fastfill()
                return
            if not stop_ticking and prev_stop and break_pct > 0.5:
                self._trigger_recover_fastfill()
                return
            # Safety: if recovering for >14s, force fast-fill.
            if self._recover_interpolating and now - self._recover_start_ts > 14.0:
                self._trigger_recover_fastfill()
                return

            # Re-break guard: if break% drops ≤5% and peak was >50%.
            if break_pct <= 0.05 and self._recover_raw_peak_pct > 0.50:
                self._cancel_recover()
                self._enter_broken()
                return

        elif self._break_state == 'refilled':
            # Wait for refilled hold timer to expire.
            if self._refill_completed_ts > 0 and now >= self._refill_completed_ts:
                self._break_state = 'normal'
                self._refill_completed_ts = now  # record for post-refill cooldown
                self._refills_completed = (self._refills_completed or 0) + 1
                self._break_ever_had_gauge = False

    def _advance_recovery(self, now: float) -> None:
        """Advance the recovering sub-phase interpolation (called from _advance)."""
        if self._break_state != 'recovering' or not self._recover_interpolating:
            return

        phase = self._recover_phase

        if phase == 'filling':
            elapsed = now - self._recover_start_ts
            t = min(1.0, elapsed / self.RECOVER_PHASE1_S)
            ease = 1.0 - (1.0 - t) ** 2  # ease-out quadratic
            pct = ease * self.RECOVER_CAP_PCT
            self._recover_current_pct = pct
            self._disp_break_pct = pct
            self._target_break_pct = pct
            if t >= 1.0:
                self._recover_phase = 'capped'
                self._recover_current_pct = self.RECOVER_CAP_PCT
                self._recover_cap_timer = now + self.RECOVER_CAP_HOLD_S

        elif phase == 'capped':
            self._disp_break_pct = self.RECOVER_CAP_PCT
            self._target_break_pct = self.RECOVER_CAP_PCT
            if self._recover_cap_timer > 0 and now >= self._recover_cap_timer:
                self._recover_phase = 'slowtail'
                self._slowtail_start_ts = now
                self._slowtail_start_pct = self._recover_current_pct
                self._recover_cap_timer = 0.0

        elif phase == 'slowtail':
            elapsed = now - self._slowtail_start_ts
            t = min(1.0, elapsed / self.RECOVER_SLOWTAIL_S)
            pct = self._slowtail_start_pct + (self.RECOVER_SLOWTAIL_PCT - self._slowtail_start_pct) * t
            self._recover_current_pct = pct
            self._disp_break_pct = pct
            self._target_break_pct = pct
            # Slowtail doesn't auto-complete to 100%; it waits at 98% for
            # a fast-fill trigger.

        elif phase == 'fastfill':
            elapsed = now - self._fastfill_start_ts
            t = min(1.0, elapsed / self.RECOVER_FASTFILL_S)
            ease = 1.0 - (1.0 - t) ** 3  # cubic ease-out
            pct = self._fastfill_start_pct + (1.0 - self._fastfill_start_pct) * ease
            self._recover_current_pct = pct
            self._disp_break_pct = pct
            self._target_break_pct = pct
            if t >= 1.0:
                self._recover_interpolating = False
                self._recover_phase = 'idle'
                self._enter_refilled()

    def _trigger_flash(self, flash_type: str) -> None:
        """Start a flash overlay (webview-parity 5-type flash system)."""
        self._flash_type = flash_type
        self._flash_start = time.time()

    def trigger_break_effect(self, effect_type: str) -> None:
        """TCP-driven break event (called from sao_gui.py)."""
        if not self._visible:
            return
        if effect_type in ('enter_breaking', 'into_fracture_state'):
            self._tcp_break_triggered = True
            self._tcp_break_ts = time.time()
            if self._break_state in ('normal', 'refilled'):
                self._enter_broken()
            elif self._break_state == 'recovering':
                self._cancel_recover()
                self._enter_broken()
        elif effect_type == 'shield_broken':
            self._trigger_shield_fx('breaking')
        elif effect_type == 'super_armor_broken':
            self._trigger_break_burst_fx()

    # ──────────────────────────────────────────
    #  Tick
    # ──────────────────────────────────────────

    def _schedule_tick(self, immediate: bool = False) -> None:
        if not self._visible or self._win is None:
            return
        if not self._registered:
            try:
                _get_scheduler(self.root).register(
                    'bosshp', self._tick, self._is_animating,
                )
                self._registered = True
            except Exception as exc:
                print(f'[BOSSHP-OV] scheduler register error: {exc}')

    def _cancel_tick(self) -> None:
        if self._registered:
            try:
                _get_scheduler(self.root).unregister('bosshp')
            except Exception:
                pass
            self._registered = False

    def _is_animating(self) -> bool:
        # v2.2.14: real animation check (was hard-coded ``return True``,
        # which forced 60 Hz compose + commit even on a steady boss bar
        # at full HP — the single biggest idle-CPU drain).
        if not self._visible:
            return False
        # Fade in/out
        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            return True
        # Tweens
        if abs(self._disp_hp_pct - self._target_hp_pct) > 4e-4:
            return True
        if abs(self._disp_trail_pct - self._target_trail_pct) > 4e-4:
            return True
        if abs(self._disp_shield_pct - self._target_shield_pct) > 4e-4:
            return True
        if abs(self._disp_break_pct - self._target_break_pct) > 4e-4:
            return True
        # Continuous animations only while their state is active.
        if self._shield_active and self._disp_shield_pct > 0.01:
            return True  # shield light sweep
        if self._in_overdrive:
            return True  # overdrive pulse
        if self._breaking_stage > 0:
            return True  # break-row scanline / bar
        # Time-bound FX windows.
        now = time.time()
        if self._damage_flash_start and (now - self._damage_flash_start) < self.DAMAGE_FLASH:
            return True
        if self._break_burst_start and (now - self._break_burst_start) < self.BREAK_BURST_FX_S:
            return True
        if self._shield_break_start and (now - self._shield_break_start) < self.SHIELD_VFX_BREAK_S:
            return True
        if self._shield_vfx_mode and (now - self._shield_vfx_start) < self._shield_vfx_duration:
            return True
        if self._root_pulse_mode and (now - self._root_pulse_start) < self._root_pulse_duration:
            return True
        return False

    @_probe.decorate('ui.bosshp.tick')
    def _tick(self, now: Optional[float] = None) -> None:
        if not self._visible or self._win is None:
            return
        if now is None:
            now = time.time()
        dt = min(0.1, max(0.001, now - (self._last_render_t or now)))
        self._last_render_t = now

        self._advance(now, dt)

        # ── Async render pipeline ──
        if self._hwnd or self._gpu_managed:
            # v2.2.23: don't let vision capture starve our commits.
            fb = self._render_worker.take_result(allow_during_capture=True)
            if fb is not None:
                if self._gpu_managed and self._gpu_presenter is not None \
                        and self._gpu_window is not None:
                    try:
                        if (fb.x, fb.y) != (self._x, self._y):
                            self._gpu_window.set_geometry(
                                self._x, self._y,
                                self.WIDTH, self.HEIGHT)
                        self._gpu_presenter.set_frame(
                            fb.bgra_bytes, fb.width, fb.height)
                        self._gpu_window.request_redraw()
                        _perf_gauge('ui.bosshp.presented', 1)
                    except Exception as e:
                        print(f'[BOSSHP-OV] gpu present error: {e}')
                elif self._hwnd:
                    try:
                        submit_ulw_commit(self._hwnd, fb, allow_during_capture=True)
                        _perf_gauge('ui.bosshp.presented', 1)
                    except Exception as e:
                        print(f'[BOSSHP-OV] ulw error: {e}')
            else:
                _perf_gauge('ui.bosshp.presented', 0)

            # v2.2.14: idle short-circuit — once we've committed a
            # steady frame, stop composing/submitting until something
            # animates again. Drains-only path keeps the worker queue
            # unblocked.
            is_anim = self._is_animating()
            if not is_anim and self._idle_committed:
                _perf_gauge('ui.bosshp.skipped_idle', 1)
                return

            # In GPU mode there's no _hwnd to pass; compose still uses
            # _x/_y for FrameBuffer origin so the present-side moves.
            self._render_worker.submit(
                self.compose_frame, now,
                self._hwnd if self._hwnd else 0,
                self._x, self._y)
            _perf_gauge('ui.bosshp.submitted', 1)
            self._idle_committed = (not is_anim)

        if self._hide_after_exit and self._fade_alpha <= 0.01:
            self.destroy()

    def _decay_toward(self, cur: float, tgt: float, tween: float) -> float:
        if abs(cur - tgt) < 0.0005:
            return tgt
        k = 1.0 - pow(0.05, self.TICK_MS / 1000.0 / max(0.05, tween))
        return cur + (tgt - cur) * k

    def _advance(self, now: float, dt: float) -> bool:
        animating = False

        # Fade
        if abs(self._fade_alpha - self._fade_target) > 1e-3:
            t = (now - self._fade_start) / max(1e-3, self._fade_duration)
            k = _ease_out_cubic(t)
            self._fade_alpha = _lerp(self._fade_from, self._fade_target, k)
            # enter translate: 10 → 0
            if self._fade_target >= 1.0:
                self._enter_translate = max(0.0, 10.0 * (1.0 - k))
            else:
                self._enter_translate = -10.0 * k * 0.6
            if t < 1.0:
                animating = True
            else:
                self._fade_alpha = self._fade_target
                if self._fade_target >= 1.0:
                    self._enter_translate = 0.0

        # Trail lag
        if self._trail_pending_time and now >= self._trail_pending_time:
            self._target_trail_pct = getattr(self, '_target_trail_pct_pending',
                                             self._target_hp_pct)
            self._trail_pending_time = 0.0

        prev = (self._disp_hp_pct, self._disp_trail_pct, self._disp_shield_pct,
                self._disp_break_pct)
        self._disp_hp_pct = self._decay_toward(
            self._disp_hp_pct, self._target_hp_pct, self.HP_TWEEN)
        self._disp_trail_pct = self._decay_toward(
            self._disp_trail_pct, self._target_trail_pct, self.TRAIL_TWEEN)
        self._disp_shield_pct = self._decay_toward(
            self._disp_shield_pct, self._target_shield_pct, self.SHIELD_TWEEN)
        self._disp_break_pct = self._decay_toward(
            self._disp_break_pct, self._target_break_pct, self.BREAK_TWEEN)
        if prev != (self._disp_hp_pct, self._disp_trail_pct,
                    self._disp_shield_pct, self._disp_break_pct):
            animating = True

        # Shield light sweep (continuous while shield is up)
        if self._shield_active and self._disp_shield_pct > 0.01:
            # 3.2s period, matches CSS @keyframes shield-light-sweep
            self._shield_light_phase = (self._shield_light_phase + dt / 3.2) % 1.0
            animating = True

        # Recovering sub-phase interpolation.
        if self._recover_interpolating:
            self._advance_recovery(now)
            animating = True

        # FX timers
        if self._flash_start and \
           now - self._flash_start < 1.5:
            animating = True
        if self._break_burst_start and \
           now - self._break_burst_start < self.BREAK_BURST_FX_S:
            animating = True
        if self._shield_break_start and \
           now - self._shield_break_start < self.SHIELD_BREAK:
            animating = True
        # Break state machine running → always animating.
        if self._break_state != 'normal':
            animating = True
        # Overdrive breathes → always animating slowly
        if self._in_overdrive:
            animating = True

        return animating

    # ──────────────────────────────────────────
    #  Rendering
    # ──────────────────────────────────────────

    @_probe.decorate('ui.bosshp.render')
    def _render(self, now: float) -> None:
        if not self._hwnd:
            return
        img = self.compose_frame(now)
        try:
            _ulw_update(self._hwnd, img, self._x, self._y)
        except Exception as e:
            print(f'[BOSSHP-OV] ulw error: {e}')

    @_probe.decorate('ui.bosshp.compose')
    def compose_frame(self, now: Optional[float] = None) -> Image.Image:
        """Render one boss-HP frame to an RGBA PIL image without touching Win32."""
        if now is None:
            now = time.time()
        w, h = self.WIDTH, self.HEIGHT

        # Global Y-translate for entry animation.
        y_off = int(round(-self._enter_translate))

        # ── v2.3.0 Phase 1: full-frame cache ──────────────────────────
        frame_sig = self._compute_frame_sig(now, y_off)
        if frame_sig is not None and self._frame_cache is not None \
                and self._frame_sig == frame_sig:
            return self._frame_cache

        # ── static-layer cache (cover + corners + boss box + bar track) ──
        # Rebuild only when the translate offset changes (i.e. enter/exit
        # animation running). Saves ~1.5 ms/frame at 60 FPS.
        if self._static_cache is None or self._static_y_off != y_off:
            base = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_cover(base, y_off)
            self._draw_corners(base, y_off)
            self._draw_boss_box(base, y_off)
            self._draw_bar_track(base, y_off)
            self._static_cache = base
            self._static_y_off = y_off
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))

        pulse_active = (
            self._root_pulse_mode and self._root_pulse_start
            and (now - self._root_pulse_start) < self._root_pulse_duration
        )
        if pulse_active:
            self._draw_root_outer_pulse(img, y_off, now)

        img.alpha_composite(self._static_cache)

        if pulse_active:
            self._draw_root_cover_pulse(img, y_off, now)

        # ── layers ──
        # (cover / corners / boss box / bar track now baked into `img`)
        self._draw_hp_trail(img, y_off)
        self._draw_hp_fill(img, y_off, now)
        if self._shield_active or self._disp_shield_pct > 0.01:
            self._draw_shield(img, y_off, now)
        if self._shield_break_start and \
           now - self._shield_break_start < self.SHIELD_BREAK:
            self._draw_shield_break(img, y_off, now)
        # Flash overlay (webview-parity 5-type system)
        self._draw_flash_overlay(img, y_off, now)
        # v2.2.18 (Phase 3a): cache name-plate + HP text + source tag.
        # Each previously triggered _draw_text_shadow which builds an
        # off-screen RGBA + _gpu_blur(2) every frame; combined ~2-4 ms
        # per BOSSHP compose. With cache, steady frames just paste back.
        # v2.2.19: keep cur_q exact (the displayed digits MUST match
        # reality when hp_source=='packet'); coarsen pct-only path to
        # 100 buckets (matches the displayed `{int(pct*100)}%` text).
        if self._hp_source == 'packet' and self._total_hp > 0:
            cur_q = int(self._current_hp)
            hp_q = 0  # text is digits-driven, pct doesn't matter
        else:
            cur_q = 0
            hp_q = int(round(self._disp_hp_pct * 100))
        text_sig = (
            y_off,
            self._boss_name,
            self._hp_source,
            cur_q,
            int(self._total_hp),
            hp_q,
            bool(self._invincible),
        )
        if self._text_layer_cache is None or self._text_layer_sig != text_sig:
            tl = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            self._draw_name_plate_text(tl, y_off)
            self._draw_hp_text(tl, y_off)
            self._text_layer_cache = tl
            self._text_layer_sig = text_sig
        img.alpha_composite(self._text_layer_cache)
        if self._shield_vfx_mode and self._shield_vfx_start and \
           now - self._shield_vfx_start < self._shield_vfx_duration:
            self._draw_shield_vfx(img, y_off, now)
        self._draw_break_row(img, y_off, now)
        if self._break_vfx_mode and self._break_vfx_start and \
           now - self._break_vfx_start < self._break_vfx_duration:
            self._draw_break_vfx(img, y_off, now)
        if self._break_burst_start and \
           now - self._break_burst_start < self.BREAK_BURST_FX_S:
            self._draw_break_burst(img, y_off, now)
        self._draw_additional_units(img, y_off)
        self._clear_lower_stray_alpha(img, y_off)
        # Invincible: red inner glow. Overdrive: gold inner glow (if not invincible).
        if self._invincible:
            self._draw_invincible_glow(img, y_off, now)
        elif self._in_overdrive:
            self._draw_overdrive_glow(img, y_off, now)

        # Apply global fade (alpha multiply).
        if self._fade_alpha < 0.999:
            img = multiply_alpha_image(img, self._fade_alpha)

        if frame_sig is not None:
            self._frame_cache = img
            self._frame_sig = frame_sig
            self._frame_version += 1
            try:
                img._sao_premult_safe = True  # type: ignore[attr-defined]
                img._sao_content_version = self._frame_version  # type: ignore[attr-defined]
            except Exception:
                pass
        return img

    def _compute_frame_sig(self, now: float,
                            y_off: int) -> Optional[tuple]:
        """Quantized signature of all per-frame pixel inputs for BossHP.

        Returns None when caching would be unsafe. Quantization buckets
        are sized below the perceptual threshold so visual quality is
        unaffected.
        """
        if not self._visible:
            return None
        if self.WIDTH <= 0 or self.HEIGHT <= 0:
            return None
        # HP/trail/shield/break tweens — 200 buckets (Δ 0.5 % of bar width).
        hp_q = int(round(self._disp_hp_pct * 200))
        trail_q = int(round(self._disp_trail_pct * 200))
        shield_q = int(round(self._disp_shield_pct * 200))
        break_q = int(round(self._disp_break_pct * 200))
        # Time-bound FX windows — bucket their age to ~30 ms (60 Hz → ≤ 2 frames).
        def _age_q(start: float, dur: float) -> int:
            if not start:
                return -1
            age = now - start
            if age < 0 or age >= dur:
                return -1
            return int(age * 33.3)
        damage_q = _age_q(self._damage_flash_start, self.DAMAGE_FLASH)
        burst_q = _age_q(self._break_burst_start, self.BREAK_BURST_FX_S)
        sbreak_q = _age_q(self._shield_break_start, self.SHIELD_BREAK)
        svfx_q = _age_q(self._shield_vfx_start,
                         self._shield_vfx_duration) if self._shield_vfx_mode else -1
        bvfx_q = _age_q(self._break_vfx_start,
                         self._break_vfx_duration) if self._break_vfx_mode else -1
        rpulse_q = _age_q(self._root_pulse_start,
                          self._root_pulse_duration) if self._root_pulse_mode else -1
        # Continuous animation phases.
        # Shield sweep cycles ~1.4 s; quantize to 28 buckets (Δ 50 ms,
        # below the eye’s ability to track a soft moving highlight).
        if self._shield_active and self._disp_shield_pct > 0.01:
            shield_sweep_q = int(((now * 0.71) % 1.0) * 28)
        else:
            shield_sweep_q = -1
        # Overdrive pulse ~ sin at 4 Hz → quantize to 32 phase buckets.
        if self._in_overdrive:
            od_q = int(((now * 4.0) % 1.0) * 32)
        else:
            od_q = -1
        # Break-row scanline / bar (driven internally by stage > 0).
        if self._breaking_stage > 0:
            br_phase_q = int(((now * 1.6) % 1.0) * 24)
        else:
            br_phase_q = -1
        additional_sig = tuple(
            (
                str(u.get('name') or ''),
                int(round(float(u.get('hp_pct') or 0.0) * 100)),
                int(round(float(u.get('extinction_pct') or 0.0) * 100)),
                bool(u.get('has_break_data', False)),
                int(u.get('breaking_stage') or -1),
                bool(u.get('shield_active', False)),
                int(round(float(u.get('shield_pct') or 0.0) * 100)),
            )
            for u in self._additional_units
        )
        return (
            int(self.WIDTH), int(self.HEIGHT), y_off,
            self._boss_name, self._hp_source,
            int(self._current_hp), int(self._total_hp),
            hp_q, trail_q, shield_q, break_q,
            bool(self._shield_active),
            int(self._breaking_stage),
            bool(self._in_overdrive),
            bool(self._invincible),
            int(round(self._fade_alpha * 100)),
            damage_q, burst_q, sbreak_q,
            svfx_q, bvfx_q, rpulse_q,
            self._shield_vfx_mode, self._break_vfx_mode, self._root_pulse_mode,
            shield_sweep_q, od_q, br_phase_q,
            additional_sig,
        )

    def _draw_root_outer_pulse(self, img: Image.Image, y_off: int,
                               now: float) -> None:
        age = now - self._root_pulse_start
        dur = max(0.001, self._root_pulse_duration)
        t = max(0.0, min(1.0, age / dur))
        mode = self._root_pulse_mode
        is_outer = mode in ('shield-break', 'break-hit')
        if not is_outer:
            return

        if mode == 'shield-break':
            if t < 0.12:
                bloom = 0.82 * (t / 0.12)
            elif t < 0.48:
                bloom = 0.82 * (1.0 - (t - 0.12) / 0.36 * 0.52)
            else:
                bloom = 0.40 * (1.0 - (t - 0.48) / 0.52)
            ring_t = min(1.0, age / max(0.001, self.ROOT_PULSE_SHIELD_BREAK_S * 0.472))
        else:
            if t < 0.10:
                bloom = 0.86 * (t / 0.10)
            elif t < 0.42:
                bloom = 0.86 * (1.0 - (t - 0.10) / 0.32 * 0.56)
            else:
                bloom = 0.38 * (1.0 - (t - 0.42) / 0.58)
            ring_t = min(1.0, age / max(0.001, self.ROOT_PULSE_BREAK_HIT_S * 0.528))

        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(layer, 'RGBA')
        if bloom > 0.01:
            bloom_box = (
                self.PANEL_X - 28,
                self.PANEL_Y - 14 + y_off,
                self.PANEL_X + self.PANEL_W + 28,
                self.PANEL_Y + self.PANEL_H + 14 + y_off,
            )
            bloom_color = (236, 253, 255, int(56 * bloom))
            draw.rounded_rectangle(bloom_box, radius=16, fill=bloom_color)
            layer = _gpu_blur(layer, 7)
        img.alpha_composite(layer)

        if ring_t < 1.0:
            ring = Image.new('RGBA', img.size, (0, 0, 0, 0))
            rd = ImageDraw.Draw(ring, 'RGBA')
            scale = 0.92 + 0.16 * ring_t
            cx = self.PANEL_X + self.PANEL_W / 2.0
            cy = self.PANEL_Y + self.PANEL_H / 2.0 + y_off
            half_w = (self.PANEL_W / 2.0 + 20) * scale
            half_h = (self.PANEL_H / 2.0 + 10) * scale
            ring_box = (
                int(round(cx - half_w)),
                int(round(cy - half_h)),
                int(round(cx + half_w)),
                int(round(cy + half_h)),
            )
            alpha = int(58 * max(0.0, (1.0 - ring_t) ** 1.2))
            rd.rounded_rectangle(
                ring_box,
                radius=max(12, int(round(12 * scale))),
                outline=(212, 248, 255, alpha),
                width=1,
            )
            img.alpha_composite(ring)

    def _draw_root_cover_pulse(self, img: Image.Image, y_off: int,
                               now: float) -> None:
        age = now - self._root_pulse_start
        dur = max(0.001, self._root_pulse_duration)
        t = max(0.0, min(1.0, age / dur))
        mode = self._root_pulse_mode

        if mode == 'shield-break':
            glow = (118, 232, 255)
            strength = 0.26 if t < 0.10 else 0.34 if t < 0.42 else 0.18
        elif mode == 'shield-restore':
            glow = (118, 232, 255)
            strength = 0.30 if t < 0.18 else 0.10
        elif mode == 'break-hit':
            glow = (104, 228, 255)
            strength = 0.30 if t < 0.10 else 0.30 if t < 0.40 else 0.12
        else:
            glow = (255, 224, 132)
            strength = 0.24 if t < 0.18 else 0.08

        overlay = Image.new('RGBA', img.size, (0, 0, 0, 0))
        od = ImageDraw.Draw(overlay, 'RGBA')
        od.rounded_rectangle(
            (
                self.PANEL_X + 2,
                self.PANEL_Y + 2 + y_off,
                self.PANEL_X + self.PANEL_W - 3,
                self.PANEL_Y + self.PANEL_H - 3 + y_off,
            ),
            radius=8,
            fill=(glow[0], glow[1], glow[2], int(42 * strength)),
        )
        overlay = _gpu_blur(overlay, 5)
        mask = self._build_cover_mask()
        if y_off:
            mask = mask.transform(
                mask.size, Image.AFFINE, (1, 0, 0, 0, 1, -y_off),
                fillcolor=0,
            )
        img.alpha_composite(_clip_alpha(overlay, mask))

    # ── cover ───────────────────────────────────────────────────────

    def _build_cover_mask(self) -> Image.Image:
        if hasattr(self, '_cover_mask_cached'):
            return self._cover_mask_cached
        mask = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (self.PANEL_X, self.PANEL_Y,
             self.PANEL_X + self.PANEL_W - 1,
             self.PANEL_Y + self.PANEL_H - 1),
            radius=10, fill=255,
        )
        self._cover_mask_cached = mask
        return mask

    def _draw_cover(self, img: Image.Image, y_off: int) -> None:
        w, h = self.WIDTH, self.HEIGHT

        # 3-stop 175deg CSS gradient ported to a vertical blend with the same
        # mid-stop at 48%.
        grad = np.zeros((h, 1, 4), dtype=np.uint8)
        ys = np.linspace(0, 1, h)
        for chan, src_a, src_b, src_c in (
            (0, self.COVER_A[0], self.COVER_MID[0], self.COVER_B[0]),
            (1, self.COVER_A[1], self.COVER_MID[1], self.COVER_B[1]),
            (2, self.COVER_A[2], self.COVER_MID[2], self.COVER_B[2])):
            upper = src_a + (src_b - src_a) * np.clip(ys / 0.48, 0, 1)
            lower = src_b + (src_c - src_b) * np.clip((ys - 0.48) / 0.52, 0, 1)
            grad[:, 0, chan] = np.where(ys <= 0.48, upper, lower)
        grad[:, 0, 3] = 255
        cover = Image.fromarray(grad, 'RGBA').resize((w, h))

        mask = self._build_cover_mask()
        if y_off:
            mask = mask.transform(
                mask.size, Image.AFFINE, (1, 0, 0, 0, 1, -y_off),
                fillcolor=0,
            )

        # CSS 2px 4px 18px rgba(20, 24, 40, 0.18):
        # offset (2, 3) down-right, blur-radius 18 → sigma ≈ 9.
        shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow)
        sd.rounded_rectangle(
            (self.PANEL_X + 2, self.PANEL_Y + 3 + y_off,
             self.PANEL_X + self.PANEL_W + 2,
             self.PANEL_Y + self.PANEL_H + 3 + y_off),
            radius=10, fill=(20, 24, 40, 46),
        )
        shadow = _gpu_blur(shadow, 6)
        # Clip shadow where content mask is opaque so shadow never
        # darkens the panel surface.
        sh_arr = np.array(shadow, dtype=np.uint8)
        sh_arr[:, :, 3] = (sh_arr[:, :, 3].astype(np.uint16)
                           * (255 - np.asarray(mask, dtype=np.uint8))
                           // 255).astype(np.uint8)
        # The overlay canvas is taller than the main bar so mini unit panels
        # can live below it. Keep the main-panel drop shadow from filling that
        # added transparent area with a broad gray rectangle.
        shadow_cut_y = max(0, min(h, int(self.ADD_Y + y_off - 6)))
        if shadow_cut_y < h:
            sh_arr[shadow_cut_y:, :, 3] = 0
        img.alpha_composite(Image.fromarray(sh_arr, 'RGBA'))

        # Paste clipped gradient
        img.paste(cover, (0, 0), mask)

        # CSS inset 0 0 22px rgba(255,255,255,0.12) — soft inner glow
        # leaking from the rim toward the centre.
        _apply_inset_shadow(
            img, mask, (255, 255, 255), alpha=31, blur_radius=11.0,
        )
        # CSS inset 0 1px 0 rgba(255,255,255,0.34) — crisp 1px top hi-lite.
        top_hi = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(top_hi).rectangle(
            (self.PANEL_X, self.PANEL_Y + y_off,
             self.PANEL_X + self.PANEL_W - 1, self.PANEL_Y + y_off),
            fill=(255, 255, 255, 87),
        )
        img.alpha_composite(_clip_alpha(top_hi, mask))

        # Subtle scan lines (CSS repeating-linear-gradient, white alpha 0.04).
        overlay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        od = ImageDraw.Draw(overlay)
        # 1 px scanline every 3 px, matching the CSS repeating-linear-gradient.
        for yy in range(
            self.PANEL_Y + y_off, self.PANEL_Y + self.PANEL_H + y_off, 3):
            od.line(
                (self.PANEL_X, yy, self.PANEL_X + self.PANEL_W - 1, yy),
                fill=(255, 255, 255, 10),
            )
        img.alpha_composite(_clip_alpha(overlay, mask))

        # Border
        draw = ImageDraw.Draw(img, 'RGBA')
        draw.rounded_rectangle(
            (self.PANEL_X, self.PANEL_Y + y_off,
             self.PANEL_X + self.PANEL_W - 1,
             self.PANEL_Y + self.PANEL_H - 1 + y_off),
            radius=10, outline=self.COVER_EDGE, width=1,
        )
        draw.rounded_rectangle(
            (self.PANEL_X + 1, self.PANEL_Y + 1 + y_off,
             self.PANEL_X + self.PANEL_W - 2,
             self.PANEL_Y + self.PANEL_H - 2 + y_off),
            radius=9, outline=self.COVER_EDGE_DEEP, width=1,
        )

    def _draw_corners(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        # Top-left cyan bracket (L-shape 16×16, 1.5 px stroke)
        tlx = self.PANEL_X + 2
        tly = self.PANEL_Y + 2 + y_off
        draw.line((tlx, tly, tlx + 16, tly), fill=self.CYAN, width=2)
        draw.line((tlx, tly, tlx, tly + 16), fill=self.CYAN, width=2)
        draw.rounded_rectangle((tlx - 1, tly - 1, tlx + 1, tly + 1), radius=1, fill=self.CYAN)
        # Bottom-right gold bracket
        brx = self.PANEL_X + self.PANEL_W - 3
        bry = self.PANEL_Y + self.PANEL_H - 3 + y_off
        draw.line((brx, bry, brx - 16, bry), fill=self.GOLD_CORNER, width=2)
        draw.line((brx, bry, brx, bry - 16), fill=self.GOLD_CORNER, width=2)
        draw.rounded_rectangle((brx - 1, bry - 1, brx + 1, bry + 1), radius=1, fill=self.GOLD_CORNER)

    # ── boss-box name plate ─────────────────────────────────────────

    def _draw_boss_box(self, img: Image.Image, y_off: int) -> None:
        """Faithful port of web/boss_hp.html xt_left + xt_right + number_xt."""
        draw = ImageDraw.Draw(img, 'RGBA')
        bx = self.BOX_X
        by = self.BOX_Y + y_off

        # xt_left
        left_poly = [
            (bx, by),
            (bx + 26, by),
            (bx + 26, by + self.BOX_H),
            (bx, by + self.BOX_H),
            (bx, by + int(self.BOX_H * 0.75)),
            (bx + 13, by + int(self.BOX_H * 0.75)),
            (bx + 13, by + int(self.BOX_H * 0.25)),
            (bx, by + int(self.BOX_H * 0.25)),
        ]

        rx0 = bx + 29
        rx1 = bx + self.BOX_W
        rw = rx1 - rx0
        rh = self.BOX_H

        right_poly_abs = [
            (rx0 + 85, by + int(round(rh * 0.22))),
            (rx0 + rw, by + int(round(rh * 0.22))),
            (rx0 + rw, by),
            (rx0, by),
            (rx0, by + rh),
            (rx0 + 228, by + rh),
            (rx0 + 234, by + int(round(rh * 0.77))),
            (rx0 + rw, by + int(round(rh * 0.77))),
            (rx0 + rw, by + int(round(rh * 0.60))),
            (rx0 + 233, by + int(round(rh * 0.60))),
            (rx0 + 228, by + int(round(rh * 0.77))),
            (rx0 + 85, by + int(round(rh * 0.77))),
        ]

        shell_shadow = Image.new('RGBA', img.size, (0, 0, 0, 0))
        ssd = ImageDraw.Draw(shell_shadow, 'RGBA')
        ssd.polygon(_offset_poly(left_poly, 2, 4), fill=(24, 30, 42, 32))
        ssd.polygon(_offset_poly(right_poly_abs, 2, 4), fill=(24, 30, 42, 30))
        img.alpha_composite(_gpu_blur(shell_shadow, 4))

        draw.polygon(left_poly, fill=self.BOX_BG)

        # xt_right
        right_fill = np.zeros((rh, rw, 4), dtype=np.uint8)
        xs = np.linspace(0.0, 1.0, rw)
        alpha = np.where(xs <= 0.5, 255.0, (1.0 - xs) / 0.5 * 255.0)
        right_fill[:, :, 0] = self.BOX_BG[0]
        right_fill[:, :, 1] = self.BOX_BG[1]
        right_fill[:, :, 2] = self.BOX_BG[2]
        right_fill[:, :, 3] = np.clip(alpha, 0, 255).astype(np.uint8)
        right_img = Image.fromarray(right_fill, 'RGBA')
        right_mask = Image.new('L', (rw, rh), 0)
        right_poly = [
            (85, int(round(rh * 0.22))),
            (rw, int(round(rh * 0.22))),
            (rw, 0),
            (0, 0),
            (0, rh),
            (228, rh),
            (234, int(round(rh * 0.77))),
            (rw, int(round(rh * 0.77))),
            (rw, int(round(rh * 0.60))),
            (233, int(round(rh * 0.60))),
            (228, int(round(rh * 0.77))),
            (85, int(round(rh * 0.77))),
        ]
        ImageDraw.Draw(right_mask).polygon(right_poly, fill=255)
        img.alpha_composite(_clip_alpha(right_img, right_mask), (rx0, by))
        right_fx = Image.new('RGBA', (rw, rh), (0, 0, 0, 0))
        rfd = ImageDraw.Draw(right_fx, 'RGBA')
        rfd.polygon(
            [(85, 2), (rw - 8, 2), (rw - 12, 12), (90, 12)],
            fill=(255, 255, 255, 12),
        )
        img.alpha_composite(_clip_alpha(right_fx, right_mask), (rx0, by))

        # number_xt
        nx = self.NUMBER_X
        ny = self.NUMBER_Y + y_off
        shadow = Image.new('RGBA', img.size, (0, 0, 0, 0))
        shadow_mask = Image.new('L', (self.NUMBER_W, self.NUMBER_H), 0)
        ImageDraw.Draw(shadow_mask).polygon(
            [(14, 0), (self.NUMBER_W, 0), (self.NUMBER_W, self.NUMBER_H),
             (0, self.NUMBER_H), (0, int(round(self.NUMBER_H * 0.58)))],
            fill=255,
        )
        shadow_box = Image.new('RGBA', (self.NUMBER_W, self.NUMBER_H), (0, 0, 0, 22))
        shadow.alpha_composite(_clip_alpha(shadow_box, shadow_mask), (nx - 4, ny + 3))
        img.alpha_composite(_gpu_blur(shadow, 5))

        number_bg = Image.new('RGBA', (self.NUMBER_W, self.NUMBER_H), self.BOX_BG)
        img.alpha_composite(_clip_alpha(number_bg, shadow_mask), (nx, ny))
        number_fx = Image.new('RGBA', (self.NUMBER_W, self.NUMBER_H), (0, 0, 0, 0))
        nfd = ImageDraw.Draw(number_fx, 'RGBA')
        nfd.polygon(
            [(14, 1), (self.NUMBER_W - 6, 1), (self.NUMBER_W - 10, 8), (18, 8)],
            fill=(255, 255, 255, 14),
        )
        img.alpha_composite(_clip_alpha(number_fx, shadow_mask), (nx, ny))

        # svg_border + divider
        ox = self.BOX_X + 108
        oy = self.BOX_Y + 9 + y_off
        outline = [
            (ox + 0, oy + 0),
            (ox + 350, oy + 0),
            (ox + 345, oy + 19),
            (ox + 145, oy + 19),
            (ox + 141, oy + 27),
            (ox + 0, oy + 27),
        ]
        draw.line(outline + [outline[0]], fill=self.LINE, width=1)
        draw.line((ox, oy, ox, oy + 25), fill=self.LINE, width=1)

    def _draw_name_plate_text(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        name = self._boss_name
        font = _pick_font(name, 15)
        ty = self.BOX_Y + 4 + y_off
        color = self.TEXT_MAIN
        max_w = 85
        name = _truncate(draw, name, font, max_w)
        tx = self.BOX_X + 29 + 10 + max(0, (85 - _text_width(draw, name, font)) // 2)
        _draw_text_shadow(
            img, (tx + 1, ty + 1), name, font,
            shadow_color=(28, 34, 42, 56), blur=2,
        )
        draw.text((tx, ty), name, fill=color, font=font)

    # ── HP bar ──────────────────────────────────────────────────────

    def _bar_mask(self) -> Image.Image:
        """The HP bar region with a diagonal cut at the right-bottom
        corner (matches xt_border clip-path). Cached — static geometry."""
        if self._cache_bar_mask is not None:
            return self._cache_bar_mask
        mask = Image.new('L', (self.WIDTH, self.HEIGHT), 0)
        d = ImageDraw.Draw(mask)
        x0 = self.BAR_X
        y0 = self.BAR_Y
        x1 = self.BAR_X + self.BAR_W
        y1 = self.BAR_Y + self.BAR_H
        # Polygon matching clip-path(0 0,100% 0,100%-5 19,145 19,141 23,0 23)
        poly = [
            (x0, y0), (x1, y0),
            (x1 - 5, y0 + 19),
            (x0 + 145, y0 + 19),
            (x0 + 141, y1),
            (x0, y1),
        ]
        d.polygon(poly, fill=255)
        self._cache_bar_mask = mask
        return mask

    def _draw_bar_track(self, img: Image.Image, y_off: int) -> None:
        track = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        td = ImageDraw.Draw(track)
        x0 = self.BAR_X
        y0 = self.BAR_Y + y_off
        x1 = self.BAR_X + self.BAR_W
        y1 = self.BAR_Y + self.BAR_H + y_off
        track_x1 = x1 - 5
        td.rectangle((x0, y0, x1, y1), fill=self.BAR_TRACK)
        # Top/bottom hairlines matching .tb_line
        td.line((x0, y0, track_x1, y0), fill=self.LINE, width=1)
        td.line((x0, y1, track_x1, y1), fill=self.LINE, width=1)
        td.line((x0, y0, x0, y1), fill=self.LINE, width=1)
        # Clip to the polygon
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(track, mask))

    def _pct_width_px(self, pct: float, width: Optional[int] = None) -> int:
        return int(math.ceil(self._pct_width_px_f(pct, width)))

    def _pct_width_px_f(self, pct: float, width: Optional[int] = None) -> float:
        # v2.2.10: float variant so HP/trail/shield/break fills can fade
        # their trailing column via subpixel_bar_width instead of stepping
        # 1 px every several frames during slow tweens.
        span = float(width or self.BAR_W)
        value = max(0.0, min(1.0, float(pct or 0.0)))
        if value <= 0.0:
            return 0.0
        if value >= 0.997:
            return span + 6.0
        return span * value + 8.0

    def _draw_hp_trail(self, img: Image.Image, y_off: int) -> None:
        pct = max(0.0, min(1.0, self._disp_trail_pct))
        if pct <= 0:
            return
        trail = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        # v2.2.10: anti-aliased trailing edge so the trail bar slides
        # smoothly behind the HP fill instead of jittering.
        tw_f = self._pct_width_px_f(pct)
        tw_int = max(1, int(math.ceil(tw_f)))
        frac = tw_f - math.floor(tw_f)
        td = ImageDraw.Draw(trail)
        if tw_int > 1:
            td.rectangle(
                (self.BAR_X, self.BAR_Y + y_off,
                 self.BAR_X + tw_int - 1, self.BAR_Y + self.BAR_H + y_off),
                fill=self.TRAIL_COLOR,
            )
        if frac > 1.0 / 512.0:
            edge_a = int(round(self.TRAIL_COLOR[3] * frac))
            if edge_a > 0:
                td.line(
                    (self.BAR_X + tw_int - 1, self.BAR_Y + y_off,
                     self.BAR_X + tw_int - 1, self.BAR_Y + self.BAR_H - 1 + y_off),
                    fill=(self.TRAIL_COLOR[0], self.TRAIL_COLOR[1],
                          self.TRAIL_COLOR[2], edge_a),
                )
        else:
            td.line(
                (self.BAR_X + tw_int - 1, self.BAR_Y + y_off,
                 self.BAR_X + tw_int - 1, self.BAR_Y + self.BAR_H - 1 + y_off),
                fill=self.TRAIL_COLOR,
            )
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(trail, mask))

    def _hp_gradient(self, pct: float) -> Tuple[Tuple[int, int, int, int],
                                                Tuple[int, int, int, int]]:
        if self._invincible:
            grey = (130, 130, 130, 220)
            return (grey, grey)
        if pct >= 0.60:
            return self.HP_GREEN
        if pct >= 0.25:
            return self.HP_YELLOW
        return self.HP_RED

    def _draw_hp_fill(self, img: Image.Image, y_off: int,
                      now: float) -> None:
        pct = max(0.0, min(1.0, self._disp_hp_pct))
        if pct <= 0:
            return
        # v2.1.17: float width + subpixel trailing column.
        fill_w = self._pct_width_px_f(pct)
        if fill_w <= 0.0:
            return
        fw_int = max(1, int(math.ceil(fill_w)))
        ca, cb = self._hp_gradient(pct)
        bar = _make_gradient_bar(fw_int, self.BAR_H, ca, cb)
        bar = subpixel_bar_width(bar, fill_w) or bar
        edge = _make_skew_cap(self.BAR_H, ca, cb, skew_px=7)
        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(bar, (self.BAR_X, self.BAR_Y + y_off))
        if pct < 0.997:
            canvas.paste(edge, (self.BAR_X + fw_int - 7, self.BAR_Y + y_off),
                         edge)
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(canvas, mask))

    def _draw_shield(self, img: Image.Image, y_off: int,
                     now: float) -> None:
        pct = max(0.0, min(1.0, self._disp_shield_pct))
        if pct <= 0.005:
            return
        sw_f = self._pct_width_px_f(pct)
        sw = max(1, int(math.ceil(sw_f)))
        shield = _make_gradient_bar(
            sw, self.BAR_H, self.SHIELD_A, self.SHIELD_B, cc=self.SHIELD_C)
        edge = _make_skew_cap(self.BAR_H, self.SHIELD_B, self.SHIELD_C,
                              skew_px=7)

        phase = self._shield_light_phase
        sweep = _make_light_sweep(sw, self.BAR_H, phase)
        shield.alpha_composite(sweep)

        scan = Image.new('RGBA', (sw, self.BAR_H), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scan, 'RGBA')
        for yy in range(2, self.BAR_H, 5):
            sd.line((0, yy, sw - 1, yy), fill=(212, 248, 255, 20), width=1)
        shield.alpha_composite(scan)
        # v2.1.17: fade trailing column for subpixel-smooth shield growth.
        shield = subpixel_bar_width(shield, sw_f) or shield

        canvas = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        canvas.paste(shield, (self.BAR_X, self.BAR_Y + y_off))
        if pct < 0.997:
            canvas.paste(edge, (self.BAR_X + sw - 7, self.BAR_Y + y_off), edge)

        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(canvas, mask))

    def _draw_shield_break(self, img: Image.Image, y_off: int,
                           now: float) -> None:
        """Webview shield breaking-ghost shell retained after shield loss."""
        age = now - self._shield_break_start
        t = age / self.SHIELD_BREAK
        if t >= 1.0:
            return
        ghost_pct = max(0.05, min(1.0, self._shield_ghost_pct or self._last_shield_pct or 0.0))
        sw = self._pct_width_px(ghost_pct)
        if sw <= 2:
            return
        shell = _make_gradient_bar(
            sw, self.BAR_H,
            (28, 74, 120, int(110 * (1.0 - t) + 24)),
            (72, 170, 235, int(148 * (1.0 - t) + 24)),
            cc=(214, 248, 255, int(56 * (1.0 - t) + 10)),
        )
        shell_scan = Image.new('RGBA', (sw, self.BAR_H), (0, 0, 0, 0))
        sdraw = ImageDraw.Draw(shell_scan, 'RGBA')
        for yy in range(3, self.BAR_H, 5):
            sdraw.line((0, yy, sw - 1, yy), fill=(182, 242, 255, int(28 * (1.0 - t))), width=1)
        shell.alpha_composite(shell_scan)

        frac = Image.new('RGBA', (sw, self.BAR_H), (0, 0, 0, 0))
        fdraw = ImageDraw.Draw(frac, 'RGBA')
        scan_x = int((-1.20 + min(1.18, t * 1.45)) * sw)
        band_w = max(18, sw // 5)
        for dx in range(band_w):
            px = scan_x + dx
            if 0 <= px < sw:
                k = 1.0 - abs(dx - band_w / 2) / max(1.0, band_w / 2)
                alpha = int(180 * max(0.0, k) * (1.0 - t * 0.55))
                fdraw.line((px, 0, px, self.BAR_H - 1), fill=(214, 248, 255, alpha), width=1)

        rng = np.random.default_rng(42)
        for _ in range(6):
            x = int(rng.integers(0, max(2, sw - 4)))
            x2 = int(x + rng.integers(-18, 18))
            fdraw.line(
                (x, 0, x2, self.BAR_H),
                fill=(255, 255, 255, int(92 * (1.0 - t))),
                width=1,
            )

        glow = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        gdraw = ImageDraw.Draw(glow, 'RGBA')
        gdraw.rounded_rectangle(
            (self.BAR_X - 2, self.BAR_Y - 2 + y_off,
             self.BAR_X + sw + 1, self.BAR_Y + self.BAR_H + 2 + y_off),
            radius=4,
            fill=(98, 208, 255, int(34 * (1.0 - t))),
        )
        img.alpha_composite(_gpu_blur(glow, 4))

        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        fx.paste(shell, (self.BAR_X, self.BAR_Y + y_off))
        fx.alpha_composite(frac, (self.BAR_X, self.BAR_Y + y_off))
        ImageDraw.Draw(fx, 'RGBA').line(
            (self.BAR_X + sw - 1, self.BAR_Y + y_off,
             self.BAR_X + sw - 1, self.BAR_Y + self.BAR_H + y_off),
            fill=(220, 250, 255, int(104 * (1.0 - t))), width=1,
        )
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(fx, mask))

    def _draw_damage_flash(self, img: Image.Image, y_off: int,
                           now: float) -> None:
        age = now - self._damage_flash_start
        t = age / self.DAMAGE_FLASH
        if t >= 1.0:
            return
        env = (1.0 - t) ** 2
        a = int(170 * env)
        if a <= 4:
            return
        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(fx).rectangle(
            (self.BAR_X, self.BAR_Y + y_off,
             self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
            fill=(255, 240, 240, a),
        )
        mask = self._bar_mask()
        if y_off:
            mask = mask.transform(mask.size, Image.AFFINE,
                                  (1, 0, 0, 0, 1, -y_off), fillcolor=0)
        img.alpha_composite(_clip_alpha(fx, mask))

    # ── text (HP numbers) ───────────────────────────────────────────

    def _draw_hp_text(self, img: Image.Image, y_off: int) -> None:
        draw = ImageDraw.Draw(img, 'RGBA')
        if self._hp_source == 'packet' and self._total_hp > 0:
            text = f'{_fmt_hp(self._current_hp)}/{_fmt_hp(self._total_hp)}'
        else:
            text = f'{int(round(self._disp_hp_pct * 100))}%'
        font = _load_font('sao', 12)
        tx = self.NUMBER_X + self.NUMBER_W - 5
        ty = self.NUMBER_Y + 2 + y_off
        tw = _text_width(draw, text, font)
        col = ((130, 130, 130, 255) if self._invincible
               else self.TEXT_MAIN)
        _draw_text_shadow(
            img, (tx - tw + 1, ty + 1), text, font,
            shadow_color=(28, 34, 42, 54), blur=2,
        )
        draw.text((tx - tw, ty), text, fill=col, font=font)

        # Source tag is left-aligned inside the fill-wrap in webview.
        tag = ('PKT' if self._hp_source == 'packet'
               else 'EST' if self._hp_source == 'estimate'
               else '')
        if tag:
            tfont = _load_font('sao', 9)
            _draw_text_shadow(
                img,
                (self.BAR_X + 9,
                 self.BAR_Y + y_off + (self.BAR_H - 9) // 2),
                tag, tfont,
                shadow_color=(28, 34, 42, 42), blur=2,
            )
            draw.text(
                (self.BAR_X + 8,
                 self.BAR_Y + y_off + (self.BAR_H - 9) // 2 - 1),
                tag, fill=self.TEXT_MUTED, font=tfont,
            )

    def _draw_shield_vfx(self, img: Image.Image, y_off: int,
                         now: float) -> None:
        age = now - self._shield_vfx_start
        dur = max(0.001, self._shield_vfx_duration)
        t = max(0.0, min(1.0, age / dur))
        mode = self._shield_vfx_mode
        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(layer, 'RGBA')
        x0 = self.BAR_X - 12
        y0 = self.BAR_Y - 14 + y_off
        w = self.BAR_W + 36
        h = self.BAR_H + 28

        travel = (-0.03 + 0.09 * t) if mode == 'breaking' else (-0.10 + 0.24 * t)
        center_x = x0 + int(round(w * (0.50 + travel)))
        band_w = max(42, int(round(w * (0.22 if mode == 'breaking' else 0.28))))
        for dx in range(-band_w, band_w + 1):
            px = center_x + dx
            if x0 <= px < x0 + w:
                k = max(0.0, 1.0 - abs(dx) / max(1.0, band_w))
                alpha = int((144 if mode == 'breaking' else 132) * k * (1.0 - t * 0.55))
                draw.line(
                    (px, y0, px, y0 + h - 1),
                    fill=(226, 252, 255, alpha), width=1,
                )

        draw.line(
            (x0 - 6, y0 + h // 2, x0 + w + 6, y0 + h // 2),
            fill=(226, 252, 255, int(168 * (1.0 - t * 0.66))), width=2,
        )
        layer = _gpu_blur(layer, 2)
        img.alpha_composite(layer)

        if mode == 'breaking':
            shards = Image.new('RGBA', img.size, (0, 0, 0, 0))
            sdraw = ImageDraw.Draw(shards, 'RGBA')
            rng = np.random.default_rng(int(self._shield_vfx_start * 1000) % 10000)
            wave = 0 if age < 0.55 else 1
            count = 6 + wave * 3
            cx = x0 + w * 0.5
            cy = y0 + h * 0.5
            for i in range(count):
                lane = i / max(1, count - 1)
                sx = x0 + w * (0.04 + lane * 0.90) + (rng.random() - 0.5) * 10
                sy = y0 + h * (0.30 + (rng.random() - 0.5) * 0.36)
                sw = 10 + rng.random() * 12
                sh = 3 + rng.random() * 4
                ang = -0.10 + (rng.random() - 0.5) * 0.72
                dist = (48 + rng.random() * 74 + wave * 12) * min(1.0, t * 1.8)
                tx = math.cos(ang) * dist + 24 * min(1.0, t * 1.2)
                ty = math.sin(ang) * dist * 0.82
                rot = (rng.random() - 0.5) * 62 * min(1.0, t * 1.4)
                alpha = int(220 * max(0.0, 0.92 - max(0.0, t - 0.10) / 0.90))
                if alpha <= 4:
                    continue
                col = (238, 253, 255, alpha) if rng.random() > 0.5 else (142, 236, 255, alpha)
                pts = _rotated_shard_polygon(sx + tx, sy + ty, sw, sh, rot)
                sdraw.polygon(pts, fill=col, outline=(226, 252, 255, min(255, alpha + 24)))
            img.alpha_composite(shards)

    def _draw_break_vfx(self, img: Image.Image, y_off: int,
                        now: float) -> None:
        age = now - self._break_vfx_start
        dur = max(0.001, self._break_vfx_duration)
        t = max(0.0, min(1.0, age / dur))
        layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(layer, 'RGBA')
        x0 = self.PANEL_X + 56
        x1 = self.PANEL_X + self.PANEL_W - 72
        cy = self.PANEL_Y + 54 + 15 + y_off
        shift = (-0.03 + 0.08 * t) if self._break_vfx_mode == 'breaking' else (-0.10 + 0.24 * t)
        center_x = int(round((x0 + x1) * 0.5 + (x1 - x0) * shift))
        band_w = max(48, int(round((x1 - x0) * 0.18)))
        for dx in range(-band_w, band_w + 1):
            px = center_x + dx
            if x0 <= px <= x1:
                k = max(0.0, 1.0 - abs(dx) / max(1.0, band_w))
                alpha = int(188 * k * (1.0 - t * 0.60))
                draw.line((px, cy - 9, px, cy + 9), fill=(255, 248, 224, alpha), width=1)
        draw.line((x0, cy, x1, cy), fill=(255, 222, 120, int(196 * (1.0 - t * 0.52))), width=2)
        layer = _gpu_blur(layer, 3)
        img.alpha_composite(layer)

    # ── break row ───────────────────────────────────────────────────

    def _draw_break_row(self, img: Image.Image, y_off: int,
                        now: float) -> None:
        if not self._has_break_data:
            return
        x0 = self.BREAK_TRACK_X
        y0 = self.BREAK_ROW_Y + y_off
        x1 = x0 + self.BREAK_TRACK_W
        draw = ImageDraw.Draw(img, 'RGBA')

        label_font = _load_font('sao', 9)
        draw.text((self.BREAK_LABEL_X, y0), 'BRK',
                  fill=(212, 156, 23, 255), font=label_font)

        bs = self._break_state
        track_h = self.BREAK_TRACK_H
        ty = y0 + 2

        # Track styling per state.
        if bs == 'broken':
            # Broken: orange border + inner glow (webview .break-track.broken)
            draw.rounded_rectangle(
                (x0, ty, x1, ty + track_h),
                radius=1, fill=(207, 208, 197, 255),
                outline=(255, 178, 72, 255), width=1,
            )
            glow_layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow_layer).rounded_rectangle(
                (x0 + 1, ty + 1, x1 - 1, ty + track_h - 1),
                radius=1, fill=(255, 140, 40, 40),
            )
            img.alpha_composite(glow_layer)
        elif bs == 'recovering':
            # Recovering: gold+cyan border + glow
            draw.rounded_rectangle(
                (x0, ty, x1, ty + track_h),
                radius=1, fill=(207, 208, 197, 255),
                outline=(255, 204, 98, 255), width=1,
            )
            glow_layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow_layer).rounded_rectangle(
                (x0 + 1, ty + 1, x1 - 1, ty + track_h - 1),
                radius=1, fill=(255, 192, 74, 30),
            )
            img.alpha_composite(glow_layer)
        else:
            draw.rounded_rectangle(
                (x0, ty, x1, ty + track_h),
                radius=1, fill=(207, 208, 197, 255),
                outline=(212, 156, 23, 255), width=1,
            )
        pct = max(0.0, min(1.0, self._disp_break_pct))
        if pct > 0:
            # v2.1.17: float fw, integer ceil for sprite sizing, fade the
            # trailing column on the base fill so slow refills/decays move
            # smoothly between pixels.
            fw_f = (x1 - x0) * pct
            fw = max(1, int(math.ceil(fw_f)))

            if bs == 'recovering':
                # Recovering fill: pulsing gradient + shimmer scan
                pulse = 0.76 + 0.24 * math.sin(
                    (now % 0.82) / 0.82 * 2 * math.pi)
                a = int(255 * pulse)
                bar = _make_gradient_bar(
                    fw, track_h,
                    (178, 138, 40, a), (222, 172, 58, a),
                    cc=(255, 232, 126, a),
                )
                bar = subpixel_bar_width(bar, fw_f) or bar
                img.alpha_composite(bar, (x0, ty))
                # Shimmer highlight scan (webview break-recover-scan)
                shimmer_phase = (now % 1.05) / 1.05
                sx_center = int(fw * shimmer_phase)
                shimmer_w = max(8, fw // 5)
                shimmer = Image.new('RGBA', (fw, track_h), (0, 0, 0, 0))
                sd = ImageDraw.Draw(shimmer)
                for dx in range(-shimmer_w // 2, shimmer_w // 2 + 1):
                    px = sx_center + dx
                    if 0 <= px < fw:
                        t = 1.0 - abs(dx) / max(1, shimmer_w // 2)
                        sa = int(100 * t * t * pulse)
                        sd.line((px, 0, px, track_h - 1),
                                fill=(255, 255, 255, sa))
                img.alpha_composite(shimmer, (x0, ty))
                # Outer glow for recovering
                glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
                ImageDraw.Draw(glow).rounded_rectangle(
                    (x0 - 1, ty - 1, x0 + fw + 1, ty + track_h + 1),
                    radius=2, fill=(243, 175, 18, 32),
                )
                img.alpha_composite(glow)
            elif bs == 'refilled' or (bs == 'normal' and pct >= 0.95):
                # Full: bright gold gradient + outer glow
                bar = _make_gradient_bar(
                    fw, track_h,
                    (255, 215, 98, 255), (255, 240, 132, 255),
                )
                bar = subpixel_bar_width(bar, fw_f) or bar
                img.alpha_composite(bar, (x0, ty))
                glow_layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
                ImageDraw.Draw(glow_layer).rounded_rectangle(
                    (x0 - 1, ty - 1, x0 + fw + 1, ty + track_h + 1),
                    radius=2, fill=(255, 224, 102, 50),
                )
                img.alpha_composite(glow_layer)
            else:
                # Normal: standard gold gradient
                bar = _make_gradient_bar(
                    fw, track_h,
                    (212, 170, 50, 255), (243, 195, 72, 255),
                )
                bar = subpixel_bar_width(bar, fw_f) or bar
                img.alpha_composite(bar, (x0, ty))

        # Percent text on the right
        if bs == 'broken':
            pct_txt = '0%'
        elif self._recover_interpolating:
            pct_txt = f'{int(round(self._recover_current_pct * 100))}%'
        else:
            pct_txt = f'{int(round(pct * 100))}%'
        tw = _text_width(draw, pct_txt, label_font)
        pct_color = ((255, 200, 80, 255) if bs == 'broken'
                     else self.TEXT_MUTED)
        draw.text(
            (self.BREAK_TEXT_X, y0), pct_txt,
            fill=pct_color, font=label_font,
        )

    def _draw_break_burst(self, img: Image.Image, y_off: int,
                          now: float) -> None:
        """Webview break-burst-layer: crack scan then HUD-slice shatter."""
        age = now - self._break_burst_start
        total_s = self.BREAK_BURST_FX_S
        if age >= total_s:
            return
        layer_x = self.PANEL_X - 30
        layer_y = self.PANEL_Y - 10 + y_off
        layer_w = 620
        layer_h = 108
        center_x = layer_x + layer_w / 2.0

        if age < self.BREAK_BURST_PHASE1_S:
            t1 = age / self.BREAK_BURST_PHASE1_S
            flash_a = 0.0
            if t1 < 0.12:
                flash_a = t1 / 0.12
            elif t1 < 0.52:
                flash_a = 1.0 - (t1 - 0.12) / 0.40 * 0.08
            else:
                flash_a = 0.84 * (1.0 - (t1 - 0.52) / 0.48)

            if flash_a > 0.01:
                mask = self._build_cover_mask()
                if y_off:
                    mask = mask.transform(
                        mask.size, Image.AFFINE, (1, 0, 0, 0, 1, -y_off),
                        fillcolor=0,
                    )
                flash = Image.new('RGBA', img.size, (0, 0, 0, 0))
                ImageDraw.Draw(flash).rounded_rectangle(
                    (self.PANEL_X, self.PANEL_Y + y_off,
                     self.PANEL_X + self.PANEL_W - 1,
                     self.PANEL_Y + self.PANEL_H - 1 + y_off),
                    radius=10,
                    fill=(255, 248, 224, int(132 * flash_a)),
                )
                img.alpha_composite(_clip_alpha(flash, mask))

            crack_layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
            cd = ImageDraw.Draw(crack_layer, 'RGBA')
            crack_sets = [
                ((255, 246, 212, int(168 * (1.0 - t1 * 0.42))), [
                    [(278, 44), (194, 20), (170, 28), (126, 12), (102, 22), (54, 16), (24, 30), (0, 28)],
                    [(282, 44), (366, 20), (390, 28), (434, 12), (458, 22), (506, 16), (536, 30), (560, 28)],
                ]),
                ((255, 201, 90, int(152 * (1.0 - t1 * 0.36))), [
                    [(280, 44), (238, 30), (218, 46), (186, 38), (164, 54), (136, 48), (108, 66), (78, 60), (46, 76)],
                    [(280, 44), (322, 30), (342, 46), (374, 38), (396, 54), (424, 48), (452, 66), (482, 60), (514, 76)],
                ]),
                ((255, 208, 106, int(136 * (1.0 - t1 * 0.50))), [
                    [(208, 10), (218, 30), (236, 44), (220, 58), (226, 80)],
                    [(352, 10), (342, 30), (324, 44), (340, 58), (334, 80)],
                ]),
            ]
            for color, lines in crack_sets:
                for pts in lines:
                    mapped = [(layer_x + x, layer_y + y) for x, y in pts]
                    cd.line(mapped, fill=color, width=2 if color[0] == 255 and color[1] > 220 else 1)

            reveal = min(1.0, t1 / 0.70)
            crack_mask = Image.new('L', img.size, 0)
            half = int(round((layer_w * 0.5) * reveal))
            ImageDraw.Draw(crack_mask).rectangle(
                (int(center_x - half), layer_y - 8, int(center_x + half), layer_y + layer_h + 8),
                fill=255,
            )
            crack_layer = _clip_alpha(crack_layer, crack_mask)
            glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
            gdraw = ImageDraw.Draw(glow, 'RGBA')
            gdraw.rounded_rectangle(
                (layer_x - 6, layer_y - 6, layer_x + layer_w + 6, layer_y + layer_h + 6),
                radius=14,
                fill=(255, 224, 124, int(28 * (1.0 - t1 * 0.22))),
            )
            img.alpha_composite(_gpu_blur(glow, 5))
            img.alpha_composite(crack_layer)

        else:
            t2 = (age - self.BREAK_BURST_PHASE1_S) / max(0.001, total_s - self.BREAK_BURST_PHASE1_S)
            glow = Image.new('RGBA', img.size, (0, 0, 0, 0))
            gdraw = ImageDraw.Draw(glow, 'RGBA')
            gdraw.rounded_rectangle(
                (layer_x - 8, layer_y - 8, layer_x + layer_w + 8, layer_y + layer_h + 8),
                radius=16,
                fill=(255, 228, 132, int(20 * max(0.0, 0.92 - t2 * 1.3))),
            )
            img.alpha_composite(_gpu_blur(glow, 6))

            shards = Image.new('RGBA', img.size, (0, 0, 0, 0))
            sdraw = ImageDraw.Draw(shards, 'RGBA')
            rng = np.random.default_rng(int(self._break_burst_start * 1000) % 10000)
            count = 18
            for i in range(count):
                grid_cols = max(3, round(count / 3))
                grid_x = (i % grid_cols) / max(1, grid_cols - 1)
                sx = layer_x + layer_w * (0.06 + grid_x * 0.88) + (rng.random() - 0.5) * 16
                sy = layer_y + layer_h * (0.15 + rng.random() * 0.70)
                sw = 8 + rng.random() * 10
                sh = 4 + rng.random() * 7
                ang = math.atan2((sy - (layer_y + layer_h * 0.5)), (sx - center_x)) + (rng.random() - 0.5) * 0.42
                dist = (60 + rng.random() * 84) * min(1.0, t2 * 1.8)
                px = sx + math.cos(ang) * dist
                py = sy + math.sin(ang) * dist * 0.94
                rot = (rng.random() - 0.5) * 88 * min(1.0, t2 * 1.6)
                alpha = 0.0
                if t2 < 0.10:
                    alpha = 0.92 * (t2 / 0.10)
                elif t2 < 0.42:
                    alpha = 0.84
                else:
                    alpha = 0.84 * max(0.0, 1.0 - (t2 - 0.42) / 0.58)
                a = int(220 * alpha)
                if a <= 4:
                    continue
                color = (255, 255, 255, a) if rng.random() > 0.5 else (255, 216, 112, a)
                pts = _rotated_shard_polygon(px, py, sw, sh, rot)
                sdraw.polygon(pts, fill=color, outline=(226, 252, 255, min(255, a + 20)))
            img.alpha_composite(shards)

    def _draw_flash_overlay(self, img: Image.Image, y_off: int,
                            now: float) -> None:
        """Webview-parity flash overlay system (5 types)."""
        if not self._flash_type or not self._flash_start:
            return
        age = now - self._flash_start
        ft = self._flash_type

        # Duration per type
        dur = 1.4 if ft in ('shield-break', 'break-burst') else 0.92
        if age >= dur:
            self._flash_type = ''
            return

        t = age / dur
        bar_mask = self._bar_mask()
        if y_off:
            bar_mask = bar_mask.transform(
                bar_mask.size, Image.AFFINE,
                (1, 0, 0, 0, 1, -y_off), fillcolor=0)

        fx = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))

        if ft == 'shield-break':
            # Cyan directional gradient (webview @keyframes shield-break)
            if t < 0.10:
                a = int(224 * (t / 0.10))
            elif t < 0.34:
                a = int(224 * (1.0 - (t - 0.10) / 0.24 * 0.36))
            elif t < 0.70:
                a = int(224 * 0.56 * (1.0 - (t - 0.34) / 0.36 * 0.68))
            else:
                a = int(224 * 0.18 * (1.0 - (t - 0.70) / 0.30))
            if a > 4:
                ImageDraw.Draw(fx).rectangle(
                    (self.BAR_X, self.BAR_Y + y_off,
                     self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
                    fill=(120, 226, 255, a),
                )
        elif ft == 'shield-restore':
            # White+cyan radial burst
            if t < 0.16:
                a = int(200 * (t / 0.16))
            elif t < 0.46:
                a = int(200 * (1.0 - (t - 0.16) / 0.30 * 0.28))
            else:
                a = int(200 * 0.72 * (1.0 - (t - 0.46) / 0.54))
            if a > 4:
                ImageDraw.Draw(fx).rectangle(
                    (self.BAR_X, self.BAR_Y + y_off,
                     self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
                    fill=(182, 245, 255, a),
                )
        elif ft == 'phase-break':
            # Gold radial burst
            if t < 0.14:
                a = int(200 * (t / 0.14))
            elif t < 0.40:
                a = int(200 * (1.0 - (t - 0.14) / 0.26 * 0.18))
            else:
                a = int(200 * 0.82 * (1.0 - (t - 0.40) / 0.60))
            if a > 4:
                ImageDraw.Draw(fx).rectangle(
                    (self.BAR_X, self.BAR_Y + y_off,
                     self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
                    fill=(255, 248, 196, a),
                )
        elif ft == 'break-burst':
            # Gold-orange directional (webview @keyframes break-burst)
            if t < 0.09:
                a = int(200 * (t / 0.09))
            elif t < 0.40:
                a = int(200 * (1.0 - (t - 0.09) / 0.31 * 0.52))
            elif t < 0.72:
                a = int(200 * 0.48 * (1.0 - (t - 0.40) / 0.32 * 0.67))
            else:
                a = int(200 * 0.16 * (1.0 - (t - 0.72) / 0.28))
            if a > 4:
                ImageDraw.Draw(fx).rectangle(
                    (self.BAR_X, self.BAR_Y + y_off,
                     self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
                    fill=(255, 214, 104, a),
                )
        elif ft == 'break-restore':
            # Gold→cyan transition
            if t < 0.14:
                a = int(200 * (t / 0.14))
            elif t < 0.42:
                a = int(200 * (1.0 - (t - 0.14) / 0.28 * 0.24))
            else:
                a = int(200 * 0.76 * (1.0 - (t - 0.42) / 0.58))
            if a > 4:
                # Transition from gold to cyan
                gold_t = max(0.0, 1.0 - t * 2.0)
                r = int(255 * gold_t + 104 * (1.0 - gold_t))
                g = int(224 * gold_t + 228 * (1.0 - gold_t))
                b = int(118 * gold_t + 255 * (1.0 - gold_t))
                ImageDraw.Draw(fx).rectangle(
                    (self.BAR_X, self.BAR_Y + y_off,
                     self.BAR_X + self.BAR_W, self.BAR_Y + self.BAR_H + y_off),
                    fill=(r, g, b, a),
                )

        if fx:
            img.alpha_composite(_clip_alpha(fx, bar_mask))

    def _clear_lower_stray_alpha(self, img: Image.Image, y_off: int) -> None:
        """Trim wide blurred FX from the lower transparent mini-panel area."""
        cut_y = int(self.PANEL_Y + self.PANEL_H + 2 + y_off)
        cut_y = max(0, min(self.HEIGHT, cut_y))
        if cut_y >= self.HEIGHT:
            return
        arr = np.array(img, dtype=np.uint8)
        keep = np.zeros((self.HEIGHT - cut_y, self.WIDTH), dtype=bool)
        units = list(self._additional_units or [])[:4]
        if units:
            n = len(units)
            total_w = n * self.ADD_W + (n - 1) * self.ADD_GAP
            start_x = int(round((self.WIDTH - total_w) / 2))
            y0 = int(self.ADD_Y + y_off - 6)
            y1 = int(self.ADD_Y + y_off + self.ADD_H + 8)
            for idx, _unit in enumerate(units):
                x0 = start_x + idx * (self.ADD_W + self.ADD_GAP) - 8
                x1 = x0 + self.ADD_W + 16
                ky0 = max(0, y0 - cut_y)
                ky1 = min(keep.shape[0], y1 - cut_y)
                kx0 = max(0, x0)
                kx1 = min(self.WIDTH, x1)
                if ky0 < ky1 and kx0 < kx1:
                    keep[ky0:ky1, kx0:kx1] = True
        lower_alpha = arr[cut_y:, :, 3]
        lower_alpha[~keep] = 0
        arr[cut_y:, :, 3] = lower_alpha
        img.paste(Image.fromarray(arr, 'RGBA'))

    def _draw_additional_units(self, img: Image.Image, y_off: int) -> None:
        units = list(self._additional_units or [])[:4]
        if not units:
            return

        n = len(units)
        total_w = n * self.ADD_W + (n - 1) * self.ADD_GAP
        start_x = int(round((self.WIDTH - total_w) / 2))
        y = self.ADD_Y + y_off
        shadow = Image.new('RGBA', img.size, (0, 0, 0, 0))
        sd = ImageDraw.Draw(shadow, 'RGBA')
        for idx, unit in enumerate(units):
            x = start_x + idx * (self.ADD_W + self.ADD_GAP)
            poly = [
                (x + 8, y), (x + self.ADD_W - 8, y),
                (x + self.ADD_W, y + 8), (x + self.ADD_W, y + self.ADD_H),
                (x + 8, y + self.ADD_H), (x, y + self.ADD_H - 8),
            ]
            sd.polygon(_offset_poly(poly, 2, 4), fill=(22, 28, 36, 46))
        img.alpha_composite(_gpu_blur(shadow, 4))

        draw = ImageDraw.Draw(img, 'RGBA')
        for idx, unit in enumerate(units):
            x = start_x + idx * (self.ADD_W + self.ADD_GAP)
            poly = [
                (x + 8, y), (x + self.ADD_W - 8, y),
                (x + self.ADD_W, y + 8), (x + self.ADD_W, y + self.ADD_H),
                (x + 8, y + self.ADD_H), (x, y + self.ADD_H - 8),
            ]
            mask = Image.new('L', (self.ADD_W, self.ADD_H), 0)
            local_poly = [(px - x, py - y) for px, py in poly]
            ImageDraw.Draw(mask).polygon(local_poly, fill=255)

            bg = Image.new('RGBA', (self.ADD_W, self.ADD_H), (0, 0, 0, 0))
            bg_arr = np.zeros((self.ADD_H, self.ADD_W, 4), dtype=np.uint8)
            top = np.array(self.COVER_A, dtype=np.float32)
            bot = np.array(self.COVER_B, dtype=np.float32)
            for yy in range(self.ADD_H):
                t = yy / max(1, self.ADD_H - 1)
                bg_arr[yy, :, :] = (top * (1.0 - t) + bot * t).astype(np.uint8)
            bg = Image.fromarray(bg_arr, 'RGBA')
            img.alpha_composite(_clip_alpha(bg, mask), (x, y))

            draw.line(poly + [poly[0]], fill=(104, 228, 255, 188), width=1)
            draw.line((x + 4, y + 4, x + 18, y + 4), fill=(255, 255, 255, 92), width=1)
            draw.line((x + self.ADD_W - 20, y + self.ADD_H - 4,
                       x + self.ADD_W - 5, y + self.ADD_H - 4),
                      fill=(243, 175, 18, 160), width=1)

            hp_pct = max(0.0, min(1.0, float(unit.get('hp_pct') or 0.0)))
            hp_x = x + 10
            hp_y = y + 5
            hp_w = self.ADD_W - 20
            hp_h = 11
            draw.rounded_rectangle((hp_x, hp_y, hp_x + hp_w, hp_y + hp_h),
                                   radius=2, fill=(54, 70, 78, 56))
            fill_w = int(round(hp_w * hp_pct))
            if fill_w > 0:
                bar = Image.new('RGBA', (fill_w, hp_h), (0, 0, 0, 0))
                arr = np.zeros((hp_h, fill_w, 4), dtype=np.uint8)
                for xx in range(fill_w):
                    t = xx / max(1, fill_w - 1)
                    arr[:, xx, 0] = int(94 * (1 - t) + 15 * t)
                    arr[:, xx, 1] = int(220 * (1 - t) + 255 * t)
                    arr[:, xx, 2] = int(255 * (1 - t) + 170 * t)
                    arr[:, xx, 3] = 232
                bar = Image.fromarray(arr, 'RGBA')
                img.alpha_composite(bar, (hp_x, hp_y))
            if bool(unit.get('shield_active')) and float(unit.get('shield_pct') or 0.0) > 0:
                shield_w = int(round(hp_w * max(0.0, min(1.0, float(unit.get('shield_pct') or 0.0)))))
                if shield_w > 0:
                    draw.rounded_rectangle((hp_x, hp_y, hp_x + shield_w, hp_y + hp_h),
                                           radius=2, fill=(98, 208, 255, 88))

            name = str(unit.get('name') or 'Unit')
            name_font = _pick_font(name, 10)
            pct_font = _pick_font(str(int(round(hp_pct * 100))), 10)
            name = _truncate(draw, name, name_font, self.ADD_W - 56)
            draw.text((x + 12, y + 4), name, font=name_font, fill=self.TEXT_MAIN)
            pct = f'{int(round(hp_pct * 100))}%'
            draw.text((x + self.ADD_W - 10 - _text_width(draw, pct, pct_font), y + 4),
                      pct, font=pct_font, fill=(80, 174, 216, 255))

            break_y = y + 23
            draw.rectangle((x + 1, break_y, x + self.ADD_W - 1, y + self.ADD_H - 1),
                           fill=(35, 38, 45, 214))
            if bool(unit.get('has_break_data', False)):
                ext_pct = max(0.0, min(1.0, float(unit.get('extinction_pct') or 0.0)))
                break_w = int(round((self.ADD_W - 20) * ext_pct))
                if break_w > 0:
                    fill = (255, 94, 94, 225)
                    if int(unit.get('breaking_stage') or -1) > 0:
                        fill = (243, 175, 18, 228)
                    draw.rounded_rectangle((x + 10, break_y + 3,
                                           x + 10 + break_w, break_y + 10),
                                          radius=1, fill=fill)
            else:
                off_font = _pick_font('OFFLINE', 8)
                draw.text((x + 11, break_y + 2), 'OFFLINE',
                          font=off_font, fill=(120, 124, 130, 230))

    def _draw_invincible_glow(self, img: Image.Image, y_off: int,
                              now: float) -> None:
        """Red inner glow on cover when boss is invincible (webview .invincible)."""
        glow = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle(
            (self.PANEL_X + 2, self.PANEL_Y + 2 + y_off,
             self.PANEL_X + self.PANEL_W - 3,
             self.PANEL_Y + self.PANEL_H - 3 + y_off),
            radius=8, fill=(255, 80, 60, 36),
        )
        glow = _gpu_blur(glow, 5)
        mask = self._build_cover_mask()
        if y_off:
            mask = mask.transform(
                mask.size, Image.AFFINE, (1, 0, 0, 0, 1, -y_off),
                fillcolor=0)
        img.alpha_composite(_clip_alpha(glow, mask))

    def _draw_overdrive_glow(self, img: Image.Image, y_off: int,
                             now: float) -> None:
        """Gold inner glow on cover when in overdrive (webview .overdrive)."""
        glow = Image.new('RGBA', (self.WIDTH, self.HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rounded_rectangle(
            (self.PANEL_X + 2, self.PANEL_Y + 2 + y_off,
             self.PANEL_X + self.PANEL_W - 3,
             self.PANEL_Y + self.PANEL_H - 3 + y_off),
            radius=8, fill=(255, 180, 54, 32),
        )
        glow = _gpu_blur(glow, 5)
        mask = self._build_cover_mask()
        if y_off:
            mask = mask.transform(
                mask.size, Image.AFFINE, (1, 0, 0, 0, 1, -y_off),
                fillcolor=0)
        img.alpha_composite(_clip_alpha(glow, mask))

    # ──────────────────────────────────────────
    #  Dragging
    # ──────────────────────────────────────────

    def _gpu_event(self, x: float, y: float):
        lx = int(round(x))
        ly = int(round(y))
        return SimpleNamespace(
            x=lx, y=ly,
            x_root=int(self._x + lx),
            y_root=int(self._y + ly),
        )

    def _on_gpu_cursor_pos(self, x: float, y: float) -> None:
        return

    def _on_gpu_mouse_button(self, button: int, action: int,
                             _mods: int, x: float, y: float) -> None:
        return

    def _on_drag_start(self, ev) -> None:
        self._restore_fixed_position()

    def _on_drag_move(self, ev) -> None:
        self._restore_fixed_position()
        if self._gpu_managed and self._gpu_window is not None:
            try:
                self._gpu_window.set_geometry(
                    self._x, self._y, self.WIDTH, self.HEIGHT)
            except Exception:
                pass
        elif self._win is not None and self._win is not self:
            try:
                self._win.geometry(f'+{self._x}+{self._y}')
            except Exception:
                pass

    def _on_drag_end(self, _ev) -> None:
        self._restore_fixed_position()


# ═══════════════════════════════════════════════
#  Small drawing helpers shared by several methods
# ═══════════════════════════════════════════════

def _make_gradient_bar(w: int, h: int,
                       ca: Tuple[int, int, int, int],
                       cb: Tuple[int, int, int, int],
                       cc: Optional[Tuple[int, int, int, int]] = None
                       ) -> Image.Image:
    """Horizontal 2- or 3-stop linear gradient."""
    if w <= 0 or h <= 0:
        return Image.new('RGBA', (1, 1), (0, 0, 0, 0))
    xs = np.linspace(0, 1, w)[None, :]
    if cc is None:
        ts = xs
        rr = ca[0] + (cb[0] - ca[0]) * ts
        gg = ca[1] + (cb[1] - ca[1]) * ts
        bb = ca[2] + (cb[2] - ca[2]) * ts
        aa = ca[3] + (cb[3] - ca[3]) * ts
    else:
        rr = np.where(xs < 0.54,
                      ca[0] + (cb[0] - ca[0]) * (xs / 0.54),
                      cb[0] + (cc[0] - cb[0]) * ((xs - 0.54) / 0.46))
        gg = np.where(xs < 0.54,
                      ca[1] + (cb[1] - ca[1]) * (xs / 0.54),
                      cb[1] + (cc[1] - cb[1]) * ((xs - 0.54) / 0.46))
        bb = np.where(xs < 0.54,
                      ca[2] + (cb[2] - ca[2]) * (xs / 0.54),
                      cb[2] + (cc[2] - cb[2]) * ((xs - 0.54) / 0.46))
        aa = np.where(xs < 0.54,
                      ca[3] + (cb[3] - ca[3]) * (xs / 0.54),
                      cb[3] + (cc[3] - cb[3]) * ((xs - 0.54) / 0.46))
    arr = np.broadcast_to(
        np.stack([rr, gg, bb, aa], axis=-1), (h, w, 4)).copy()
    # Vertical shading (top brighter).
    ys = np.linspace(1.0, 0.82, h)[:, None, None]
    arr[:, :, :3] = np.clip(arr[:, :, :3] * ys, 0, 255)
    return Image.fromarray(arr.astype(np.uint8), 'RGBA')


def _make_skew_cap(h: int,
                   ca: Tuple[int, int, int, int],
                   cb: Tuple[int, int, int, int],
                   skew_px: int = 7) -> Image.Image:
    """Build the CSS `::after { right:-11px; skewX(-14deg) }` leading
    edge of the fill bar — a parallelogram that juts past the fill
    end to give the bar its signature angled tip."""
    w = 18
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Use the right-hand colour (cb) as base.
    col = (cb[0], cb[1], cb[2], cb[3])
    d.polygon(
        [(skew_px, 0), (w, 0), (w - skew_px, h), (0, h)],
        fill=col,
    )
    return img


def _rotated_shard_polygon(cx: float, cy: float, w: float, h: float,
                           deg: float) -> List[Tuple[int, int]]:
    pts = [
        (-w * 0.50, -h * 0.38),
        (w * 0.28, -h * 0.50),
        (w * 0.50, 0.0),
        (w * 0.34, h * 0.50),
        (-w * 0.50, h * 0.38),
    ]
    rad = math.radians(deg)
    cs = math.cos(rad)
    sn = math.sin(rad)
    out: List[Tuple[int, int]] = []
    for px, py in pts:
        rx = px * cs - py * sn
        ry = px * sn + py * cs
        out.append((int(round(cx + rx)), int(round(cy + ry))))
    return out


def _make_light_sweep(w: int, h: int, phase: float) -> Image.Image:
    """A skewed highlight band sweeping left→right over the shield fill.
    phase ∈ [0,1) controls its position."""
    if w <= 0 or h <= 0:
        return Image.new('RGBA', (max(w, 1), max(h, 1)), (0, 0, 0, 0))
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    band_w = max(24, w // 4)
    # band center x travels from -band_w to w+band_w
    cx = int(-band_w + phase * (w + 2 * band_w))
    # Peak brightness follows a sine envelope matching CSS sweep.
    peak = 0.6 + 0.4 * math.sin(phase * math.pi)
    arr = np.zeros((h, w, 4), dtype=np.uint8)
    for x in range(max(0, cx - band_w), min(w, cx + band_w)):
        d = abs(x - cx) / max(1, band_w)
        k = (1.0 - d) ** 2 * peak
        arr[:, x, 0] = 255
        arr[:, x, 1] = 255
        arr[:, x, 2] = 255
        arr[:, x, 3] = int(110 * k)
    return Image.fromarray(arr, 'RGBA')


def _truncate(draw: ImageDraw.ImageDraw, text: str, font,
              max_w: int) -> str:
    if not text:
        return ''
    if _text_width(draw, text, font) <= max_w:
        return text
    ell = '…'
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi) // 2
        if _text_width(draw, text[:mid] + ell, font) <= max_w:
            lo = mid + 1
        else:
            hi = mid
    return text[: max(0, lo - 1)] + ell


# ────────────────────────────────────────────────────────────
# Theme dictionaries & registration
# ────────────────────────────────────────────────────────────

BOSS_THEME_LIGHT = {
    'COVER_A':         (244, 246, 247, 255),
    'COVER_MID':       (232, 235, 238, 255),
    'COVER_B':         (223, 227, 231, 255),
    'COVER_EDGE':      (186, 190, 196, 255),
    'COVER_EDGE_DEEP': (160, 165, 171, 255),
    'LINE':            (214, 216, 219, 255),
    'LINE_SOFT':       (246, 247, 248, 255),
    'HAIRLINE_LIGHT':  (248, 249, 250, 255),
    'HAIRLINE_MID':    (226, 229, 232, 255),
    'HAIRLINE_DARK':   (160, 165, 171, 255),
    'TEXT_SHADOW':     (190, 192, 195, 255),
    'TEXT_MAIN':       (100, 99, 100, 255),
    'TEXT_MUTED':      (140, 135, 138, 255),
    'BOX_BG':          (248, 249, 250, 255),
    'BAR_TRACK':       (172, 176, 182, 42),
}

BOSS_THEME_DARK = {
    'COVER_A':         (20, 26, 36, 255),
    'COVER_MID':       (16, 20, 30, 255),
    'COVER_B':         (12, 16, 24, 255),
    'COVER_EDGE':      (50, 80, 110, 200),
    'COVER_EDGE_DEEP': (40, 65, 90, 200),
    'LINE':            (50, 70, 90, 255),
    'LINE_SOFT':       (30, 42, 58, 255),
    'HAIRLINE_LIGHT':  (35, 50, 70, 255),
    'HAIRLINE_MID':    (45, 62, 82, 255),
    'HAIRLINE_DARK':   (60, 85, 110, 255),
    'TEXT_SHADOW':     (0, 0, 0, 120),
    'TEXT_MAIN':       (210, 220, 230, 255),
    'TEXT_MUTED':      (120, 140, 160, 255),
    'BOX_BG':          (22, 30, 42, 255),
    'BAR_TRACK':       (40, 60, 80, 50),
}

from sao_theme import register_panel_theme
register_panel_theme('bosshp', 'light', BOSS_THEME_LIGHT)
register_panel_theme('bosshp', 'dark', BOSS_THEME_DARK)
