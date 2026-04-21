# -*- coding: utf-8 -*-
"""v2.2.12 — Per-pixel-alpha layered window for the SAO menu HUD.

The legacy HUD lived on a fullscreen ``Toplevel(-transparentcolor='#010101')
+ Canvas`` chroma-key surface, which has two structural problems:

1. Windows DWM does **not** vsync chroma-key composites, so any per-frame
   geometry change (the menu's breathing motion) tears.
2. The canvas-native HUD primitives (scan line, trail lines, dot ovals,
   timestamp text) are mutated on the Tk main thread every frame,
   competing with packet capture / parser work.

This module hosts ``MenuHudOverlay`` which replaces the HUD half of the
menu surface with:

- A **layered window (ULW)** — per-pixel alpha, fully click-through
  (``WS_EX_TRANSPARENT``), DWM-vsync'd composites.
- An **off-thread compose** path via ``AsyncFrameWorker`` (heavy lane,
  preferring isolation so it shares lane allocation rules with SkillFX
  and BossHP).
- The existing v2.2.11 ``submit_ulw_commit`` queue that drains on the
  dedicated ``ulw-commit`` worker thread, so the Tk main thread only
  enqueues frames.

The widget Toplevel (``SAOPopUpMenu._overlay``) keeps its chroma-key
surface for hosting actual Tk widgets (buttons, labels, frames) — those
need real Tk widgetry for accessibility / focus / IME, which ULW does
not support. The two windows are siblings; the ULW sits beneath the
widget Toplevel so widget hit-testing is unchanged.

Toggle via env ``SAO_GPU_MENU_HUD=0`` to fall back to the legacy
canvas-native HUD path. Default ON in v2.2.12.
"""
from __future__ import annotations

import ctypes
import os
import threading
import time
import tkinter as tk
from typing import Any, Optional, Tuple

from PIL import Image

from overlay_render_worker import (
    AsyncFrameWorker,
    FrameBuffer,
    submit_ulw_commit,
    drop_pending_ulw_for,
)
from perf_probe import probe as _probe
from sao_menu_hud import MenuHudSpriteRenderer
try:
    import gpu_overlay_window as _gow
except Exception:
    _gow = None  # type: ignore[assignment]

_user32 = ctypes.windll.user32

GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TOPMOST = 0x00000008
WS_EX_TRANSPARENT = 0x00000020
WS_EX_NOACTIVATE = 0x08000000


def gpu_menu_hud_enabled() -> bool:
    """Honour ``SAO_GPU_MENU_HUD`` env override; otherwise consult
    ``config.USE_GPU_MENU_HUD`` (default True in v2.2.12)."""
    env = os.environ.get('SAO_GPU_MENU_HUD')
    if env is not None:
        return env != '0'
    try:
        from config import USE_GPU_MENU_HUD  # type: ignore
        return bool(USE_GPU_MENU_HUD)
    except Exception:
        return True


class MenuHudOverlay:
    """Click-through layered window driving the menu HUD off-thread.

    Designed to mirror ``BurstReadyOverlay``'s skeleton so it slots into
    the existing ``AsyncFrameWorker`` + ``submit_ulw_commit`` pipeline
    without bespoke threading.
    """

    def __init__(self, root: tk.Tk):
        self.root = root
        self._win: Optional[tk.Toplevel] = None
        self._hwnd: int = 0
        self._visible = False
        # Last-known geometry inputs (anchor + size).
        self._anchor_x: int = 0
        self._anchor_y: int = 0
        self._content_w: int = 0
        self._content_h: int = 0
        self._screen_w: int = 0
        self._screen_h: int = 0
        # Sprite-origin offset returned by the renderer (negative pad).
        self._sprite_off: Tuple[int, int] = (0, 0)
        # Pinned worker lane (heavy / isolated) for compose work.
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)
        self._renderer = MenuHudSpriteRenderer()
        # Track last submitted commit position so we don't enqueue
        # duplicates when nothing moved.
        self._last_commit_xy: Optional[Tuple[int, int]] = None
        # v2.3.0: phase-quantized submit dedup. The breathing animation
        # advances continuously (math.sin), so naive submit-every-tick
        # pumps a fresh fullscreen PIL compose every 16.7 ms even though
        # the visual difference between adjacent 60 Hz phases is
        # invisible to the eye. We quantize the compose key to 30 Hz
        # (q_phase = round(phase * 30) / 30) and skip submit when the
        # key is unchanged. Halves worker load with zero visible change.
        self._last_submit_sig: Optional[Tuple[Any, ...]] = None
        # First-tick anchor so phase math is stable across calls.
        self._phase_t0: float = 0.0
        self._destroyed = False
        # v2.3.0 Phase 3: optional GPU overlay presentation. When
        # SAO_GPU_OVERLAY=1 the per-frame ULW commit (~1-2 ms GDI
        # bitmap copy) is replaced with a moderngl texture upload +
        # quad draw (~0.01 ms). Worker still composes PIL→BGRA the
        # same way; only the present transport changes.
        self._gpu_window: Optional[Any] = None
        self._gpu_presenter: Optional[Any] = None

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def _ensure_window(self) -> None:
        if self._win is not None:
            return
        # v2.3.0 Phase 3: GPU overlay presentation path (env-gated).
        if _gow is not None and _gow.glfw_supported():
            try:
                pump = _gow.get_glfw_pump(self.root)
                presenter = _gow.BgraPresenter()
                # Initial 1x1; tick will resize via set_geometry once
                # the first compose result lands.
                gpu_win = _gow.GpuOverlayWindow(
                    pump,
                    w=1, h=1,
                    x=int(self._anchor_x), y=int(self._anchor_y),
                    render_fn=presenter.render,
                    click_through=True,
                    title='sao_menu_hud_gpu',
                )
                gpu_win.show()
                self._gpu_window = gpu_win
                self._gpu_presenter = presenter
                self._win = self  # type: ignore[assignment]  # sentinel
                self._hwnd = 0
                self._visible = True
                return
            except Exception:
                self._gpu_window = None
                self._gpu_presenter = None
        # Initial size is a placeholder; set_geometry resizes on first tick.
        self._win = tk.Toplevel(self.root)
        self._win.overrideredirect(True)
        self._win.attributes('-topmost', True)
        self._win.geometry(f'1x1+{int(self._anchor_x)}+{int(self._anchor_y)}')
        self._win.update_idletasks()
        try:
            self._hwnd = _user32.GetParent(self._win.winfo_id()) \
                or self._win.winfo_id()
        except Exception:
            self._hwnd = self._win.winfo_id()
        ex = _user32.GetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE)
        # Per-pixel alpha + click-through + no taskbar entry.
        _user32.SetWindowLongW(
            ctypes.c_void_p(self._hwnd), GWL_EXSTYLE,
            ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW
            | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        )
        # Exclude from screen-capture (matches SAO overlay pattern).
        try:
            _user32.SetWindowDisplayAffinity(
                ctypes.c_void_p(self._hwnd), 0x00000011)
        except Exception:
            pass
        self._visible = True

    def set_geometry(self, anchor_x: int, anchor_y: int,
                     content_w: int, content_h: int,
                     screen_w: int, screen_h: int) -> None:
        """Update the HUD's anchor (top-left of the content frame in
        screen coords) and the content/screen dimensions used by the
        renderer to lay out brackets and rails.

        Cheap to call every tick — only stores values, no allocation.
        """
        self._anchor_x = int(anchor_x)
        self._anchor_y = int(anchor_y)
        self._content_w = max(120, int(content_w))
        self._content_h = max(120, int(content_h))
        self._screen_w = max(1, int(screen_w))
        self._screen_h = max(1, int(screen_h))

    # ──────────────────────────────────────────
    #  Compose (called on render lane)
    # ──────────────────────────────────────────

    @_probe.decorate('ui.menu.compose')
    def compose_frame(self, now: float, phase: float) -> Image.Image:
        """Worker-thread entry point. Returns a fresh RGBA PIL Image."""
        img, off = self._renderer.render_pil(
            self._content_w, self._content_h,
            self._screen_w, self._screen_h,
            phase,
        )
        # Stash the sprite origin offset so the main-thread tick can
        # translate the commit position correctly.
        self._sprite_off = off
        return img

    # ──────────────────────────────────────────
    #  Tick (main thread)
    # ──────────────────────────────────────────

    @_probe.decorate('ui.menu.tick')
    def tick(self, now: float, dx: int, dy: int, phase: float) -> None:
        """Drain the previous frame (if ready) and submit a new compose.

        ``dx``/``dy`` are the breathing offsets — they are added to the
        anchor position when committing, so the HUD visibly drifts
        without any window geometry mutation between commits.
        """
        if self._destroyed:
            return
        if self._win is None:
            self._ensure_window()
        # In GPU mode there's no hwnd; presence of _gpu_window is the
        # gate. ULW path keeps the legacy hwnd gate.
        if not self._hwnd and self._gpu_window is None:
            return

        # 1) Take any frame the worker finished and commit it. The
        #    AsyncFrameWorker already tagged it with the x/y we passed
        #    to submit() last tick, so submit_ulw_commit will move the
        #    layered window atomically with the new bitmap inside
        #    UpdateLayeredWindow — no separate geometry call needed.
        # v2.2.23: bypass capture-idle gate; vision PrintWindow on the
        # game HWND can't conflict with our ulw commits and would
        # otherwise drop ~30-100% of menu HUD frames during combat.
        fb = self._render_worker.take_result(allow_during_capture=True)
        if fb is not None:
            if self._gpu_presenter is not None and self._gpu_window is not None:
                # GPU path: move window + upload BGRA texture + redraw.
                # set_geometry is cheap (no realloc when size matches).
                try:
                    self._gpu_window.set_geometry(
                        fb.x, fb.y, fb.width, fb.height)
                    self._gpu_presenter.set_frame(
                        fb.bgra_bytes, fb.width, fb.height)
                    self._gpu_window.request_redraw()
                    self._last_commit_xy = (fb.x, fb.y)
                except Exception:
                    pass
            else:
                try:
                    submit_ulw_commit(self._hwnd, fb, allow_during_capture=True)
                    self._last_commit_xy = (fb.x, fb.y)
                except Exception:
                    pass

        # 2) Submit a new compose. We pass the on-screen sprite top-left
        #    via x/y so the FrameBuffer carries the right position.
        sprite_x = self._anchor_x + dx + self._sprite_off[0]
        sprite_y = self._anchor_y + dy + self._sprite_off[1]
        # v2.3.0: phase-quantized dedup. q_phase ticks at 30 Hz; below
        # that quantum, every other 60 Hz tick reuses the previous
        # composed frame (presenter keeps it on the GL surface).
        q_phase = round(phase * 30.0) / 30.0
        sig = (int(sprite_x), int(sprite_y),
               self._content_w, self._content_h,
               self._screen_w, self._screen_h, q_phase)
        if sig == self._last_submit_sig:
            return
        self._last_submit_sig = sig
        # We need the renderer's sprite_off, which is set inside
        # compose_frame on the worker thread. Use the cached value from
        # the previous compose; on the very first frame it's (0, 0)
        # which is acceptable for one frame of placeholder offset.
        try:
            self._render_worker.submit(
                self._compose, now, self._hwnd,
                int(sprite_x), int(sprite_y),
            )
        except Exception:
            pass

    def _compose(self, now: float) -> Image.Image:
        """Worker-thread entry. The phase is (now - first_call) so
        animations advance with wall-clock time and are stable across
        whatever scheduling jitter the lane sees."""
        if self._phase_t0 == 0.0:
            self._phase_t0 = now
        phase = max(0.0, now - self._phase_t0)
        return self.compose_frame(now, phase)

    # ──────────────────────────────────────────
    #  Teardown
    # ──────────────────────────────────────────

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        if self._hwnd:
            try:
                drop_pending_ulw_for(self._hwnd)
            except Exception:
                pass
        try:
            self._render_worker.stop()
        except Exception:
            pass
        # v2.3.0 Phase 3: tear down GPU resources first.
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
        self._visible = False
        self._renderer.reset()
        self._last_commit_xy = None
        self._last_submit_sig = None
