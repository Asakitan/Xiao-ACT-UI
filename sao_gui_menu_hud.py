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
from typing import Optional, Tuple

from PIL import Image

from overlay_render_worker import (
    AsyncFrameWorker,
    FrameBuffer,
    submit_ulw_commit,
    drop_pending_ulw_for,
)
from sao_menu_hud import MenuHudSpriteRenderer

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
        # First-tick anchor so phase math is stable across calls.
        self._phase_t0: float = 0.0
        self._destroyed = False

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

    def _ensure_window(self) -> None:
        if self._win is not None:
            return
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
        if not self._hwnd:
            return

        # 1) Take any frame the worker finished and commit it. The
        #    AsyncFrameWorker already tagged it with the x/y we passed
        #    to submit() last tick, so submit_ulw_commit will move the
        #    layered window atomically with the new bitmap inside
        #    UpdateLayeredWindow — no separate geometry call needed.
        fb = self._render_worker.take_result()
        if fb is not None:
            try:
                submit_ulw_commit(self._hwnd, fb)
                self._last_commit_xy = (fb.x, fb.y)
            except Exception:
                pass

        # 2) Submit a new compose. We pass the on-screen sprite top-left
        #    via x/y so the FrameBuffer carries the right position.
        sprite_x = self._anchor_x + dx + self._sprite_off[0]
        sprite_y = self._anchor_y + dy + self._sprite_off[1]
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
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._hwnd = 0
        self._visible = False
        self._renderer.reset()
        self._last_commit_xy = None
