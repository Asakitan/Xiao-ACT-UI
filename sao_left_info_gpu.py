# -*- coding: utf-8 -*-
"""v2.3.0 Phase 3+ — GPU-presented SAOLeftInfo panel.

Mirrors :mod:`sao_menu_bar_gpu`. The popup's left info panel is two
stacked Canvases (top + bottom) that paint cached PIL plates with an
optional sweep highlight on open / close / sync_pulse.

When ``SAO_GPU_LEFT_INFO`` is enabled, the Tk Canvases are kept at
chroma-key bg with no ``create_image`` so they stay invisible, and
the two plates are composed into one BGRA frame on the heavy
``AsyncFrameWorker`` lane. Presentation goes through a single
``GpuOverlayWindow`` sized to the panel's combined bounding box.
"""
from __future__ import annotations

import os
import threading
import time
import tkinter as tk
from typing import Any, Optional, Tuple

from PIL import Image

from overlay_render_worker import AsyncFrameWorker
from perf_probe import probe as _probe
from sao_menu_hud import MenuLeftInfoRenderer

try:
    import gpu_overlay_window as _gow
except Exception:  # pragma: no cover
    _gow = None  # type: ignore[assignment]


def gpu_left_info_enabled() -> bool:
    env = os.environ.get('SAO_GPU_LEFT_INFO')
    if env is not None:
        return env != '0'
    if _gow is None:
        return False
    try:
        return bool(_gow.glfw_supported())
    except Exception:
        return False


class _LeftInfoSnapshot:
    """Plain-data carrier built on the Tk thread, consumed on worker."""

    __slots__ = (
        'username', 'description',
        'top_w', 'top_h',
        'bottom_w', 'bottom_h',
        'sweep_phase', 'sweep_strength',
    )

    def __init__(self, username: str, description: str,
                 top_w: int, top_h: int,
                 bottom_w: int, bottom_h: int,
                 sweep_phase: float, sweep_strength: float):
        self.username = str(username)
        self.description = str(description)
        self.top_w = int(top_w)
        self.top_h = int(top_h)
        self.bottom_w = int(bottom_w)
        self.bottom_h = int(bottom_h)
        self.sweep_phase = float(sweep_phase)
        self.sweep_strength = float(sweep_strength)


class LeftInfoGpuPainter:
    """Owns one ``GpuOverlayWindow`` + ``AsyncFrameWorker`` for the
    full left info panel. Top + bottom plates compose into a single
    sprite each tick on the worker."""

    def __init__(self, root: tk.Tk):
        self._root = root
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)
        self._renderer = MenuLeftInfoRenderer()
        self._gpu_window: Optional[Any] = None
        self._presenter: Optional[Any] = None
        self._destroyed = False
        self._last_sig: Optional[tuple] = None
        self._last_geom: Optional[Tuple[int, int, int, int]] = None
        self._lock = threading.Lock()

    def _ensure_window(self, w: int, h: int, x: int, y: int) -> bool:
        if self._gpu_window is not None:
            return True
        if _gow is None or not _gow.glfw_supported():
            return False
        try:
            pump = _gow.get_glfw_pump(self._root)
            self._presenter = _gow.BgraPresenter()
            self._gpu_window = _gow.GpuOverlayWindow(
                pump,
                w=max(1, int(w)), h=max(1, int(h)),
                x=int(x), y=int(y),
                render_fn=self._presenter.render,
                click_through=True,
                title='sao_left_info_gpu',
            )
            self._gpu_window.show()
            return True
        except Exception:
            self._presenter = None
            self._gpu_window = None
            return False

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        try:
            self._render_worker.stop()
        except Exception:
            pass
        if self._presenter is not None:
            try:
                self._presenter.release()
            except Exception:
                pass
            self._presenter = None
        if self._gpu_window is not None:
            try:
                self._gpu_window.destroy()
            except Exception:
                pass
            self._gpu_window = None

    @_probe.decorate('ui.menu.left_info_gpu_tick')
    def tick(self, screen_x: int, screen_y: int,
             snap: _LeftInfoSnapshot) -> None:
        if self._destroyed:
            return
        # Combined bounding box: both plates are stacked vertically with
        # left-aligned anchor 'nw'; outer width is max of the two,
        # outer height is sum.
        out_w = max(1, snap.top_w, snap.bottom_w)
        out_h = max(1, snap.top_h + snap.bottom_h)
        if not self._ensure_window(out_w, out_h, screen_x, screen_y):
            return

        # 1) Drain previous result and present.
        fb = self._render_worker.take_result(allow_during_capture=True)
        if fb is not None and self._gpu_window is not None and self._presenter is not None:
            try:
                geom = (fb.x, fb.y, fb.width, fb.height)
                if geom != self._last_geom:
                    self._gpu_window.set_geometry(*geom)
                    self._last_geom = geom
                self._presenter.set_frame(fb.bgra_bytes, fb.width, fb.height)
                self._gpu_window.request_redraw()
            except Exception:
                pass

        # 2) Build dedup signature. Quantize sweep params identically
        #    to MenuLeftInfoRenderer's internal cache so we submit one
        #    frame per visually-distinct state.
        if snap.sweep_strength > 0.005:
            sp_q = round(snap.sweep_phase * 16.0) / 16.0
            ss_q = round(snap.sweep_strength * 16.0) / 16.0
        else:
            sp_q = 0.0
            ss_q = 0.0
        sig = (snap.username, snap.description,
               snap.top_w, snap.top_h,
               snap.bottom_w, snap.bottom_h,
               sp_q, ss_q)
        with self._lock:
            if sig == self._last_sig:
                return
            self._last_sig = sig

        # 3) Submit compose. Capture state by value into closure.
        s = snap
        renderer = self._renderer
        out_w_local = out_w
        out_h_local = out_h

        def compose(_now: float) -> Image.Image:
            img = Image.new('RGBA', (out_w_local, out_h_local), (0, 0, 0, 0))
            if s.top_w >= 20 and s.top_h >= 20:
                top = renderer.render_top_pil(
                    s.username, s.top_w, s.top_h,
                    sweep_phase=s.sweep_phase,
                    sweep_strength=s.sweep_strength,
                )
                img.alpha_composite(top, (0, 0))
            if s.bottom_w >= 20 and s.bottom_h >= 15:
                bot = renderer.render_bottom_pil(
                    s.description, s.bottom_w, s.bottom_h,
                    sweep_phase=s.sweep_phase,
                    sweep_strength=s.sweep_strength,
                )
                img.alpha_composite(bot, (0, s.top_h))
            return img

        try:
            self._render_worker.submit(
                compose, time.perf_counter(),
                0, int(screen_x), int(screen_y),
            )
        except Exception:
            pass
