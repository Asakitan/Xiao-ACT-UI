# -*- coding: utf-8 -*-
"""v2.3.0 Phase 3+ — GPU-presented SAOMenuBar fisheye row.

The vertical menu sidebar (``SAOMenuBar``) lives inside the chroma-key
``SAOPopUpMenu`` Toplevel. Each ``SAOCircleButton`` is a ``tk.Canvas``
that paints a per-button PIL sprite + ``ImageTk.PhotoImage`` upload +
``itemconfigure`` on the **Tk main thread** every fisheye tick. With 8
buttons that's measurably the dominant cost of ``ui.menu.fisheye_tick``
(~16 ms p99 on the user's box).

This module replaces that visual layer with:

- A single **GPU overlay window** (GLFW + ``moderngl``) sized to the
  menubar rect, sitting **on top of** the chroma-key popup. Because
  the popup's bg is ``#010101`` chroma-keyed and the SAOCircleButton
  Canvases also inherit that bg, the area occupied by the menubar is
  fully see-through, letting the GPU layer show through.
- An **off-thread compose** path that snapshots all 8 button visual
  states each tick and composites the entire strip into one BGRA frame
  on the heavy ``AsyncFrameWorker`` lane.
- The ``SAOCircleButton`` widgets remain in place as **invisible
  hit-test rectangles** — their ``_draw`` is no-op'd when the painter
  is attached, so no per-button PIL/PhotoImage work runs on the main
  thread. Click/Enter/Leave bindings still fire because the widget
  area exists.

Toggle via env ``SAO_GPU_MENU_BAR``. Defaults to whatever
``SAO_GPU_OVERLAY`` says (so users only need one switch for "all
GPU overlays on/off").
"""
from __future__ import annotations

import os
import threading
import time
import tkinter as tk
from typing import Any, Callable, List, Optional, Tuple

from PIL import Image

from overlay_render_worker import AsyncFrameWorker, FrameBuffer
from perf_probe import probe as _probe
from sao_menu_hud import MenuCircleButtonRenderer

try:
    import gpu_overlay_window as _gow
except Exception:  # pragma: no cover - optional dep
    _gow = None  # type: ignore[assignment]


def gpu_menu_bar_enabled() -> bool:
    """Honour ``SAO_GPU_MENU_BAR`` if set, otherwise mirror the master
    ``SAO_GPU_OVERLAY`` switch (so flipping one env turns on the whole
    GPU overlay family)."""
    env = os.environ.get('SAO_GPU_MENU_BAR')
    if env is not None:
        return env != '0'
    if _gow is None:
        return False
    try:
        return bool(_gow.glfw_supported())
    except Exception:
        return False


class _ButtonSnapshot:
    """Plain data carrier for a single button's visual state. Built
    on the main thread under the Tk lock and consumed on the worker —
    it must not touch any Tk widget."""

    __slots__ = ('size', 'hover_t', 'active', 'icon')

    def __init__(self, size: float, hover_t: float, active: bool, icon: str):
        self.size = float(size)
        self.hover_t = float(hover_t)
        self.active = bool(active)
        self.icon = str(icon or '●')


class MenuBarGpuPainter:
    """Owns one ``GpuOverlayWindow`` + ``AsyncFrameWorker`` for the
    SAOMenuBar fisheye strip. Composes all buttons into one BGRA
    frame per tick on the worker lane and presents via moderngl.

    The painter is a **passive** consumer of state: SAOMenuBar's
    ``_tick_float`` snapshots its buttons after updating sizes and
    feeds the snapshot in via :meth:`tick`. The painter handles its
    own no-op deduping based on the snapshot signature.
    """

    def __init__(self, root: tk.Tk, slot_px: int, max_size: int):
        self._root = root
        self._slot = int(slot_px)
        self._max_size = int(max_size)
        self._render_worker = AsyncFrameWorker(prefer_isolation=True)
        self._renderer = MenuCircleButtonRenderer()
        self._gpu_window: Optional[Any] = None
        self._presenter: Optional[Any] = None
        self._destroyed = False
        # Latest signature of the snapshot we already submitted; stops
        # us from re-rendering 60 Hz of identical frames once the bar
        # settles back to rest size.
        self._last_sig: Optional[tuple] = None
        # Cached strip dims so we only call set_geometry on real moves.
        self._last_geom: Optional[Tuple[int, int, int, int]] = None
        # Lock protects the small caches above; tick() is main-thread
        # only so this is mainly defensive against future async usage.
        self._lock = threading.Lock()

    # ──────────────────────────────────────────
    #  Lifecycle
    # ──────────────────────────────────────────

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
                title='sao_menu_bar_gpu',
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

    # ──────────────────────────────────────────
    #  Per-tick entry (main thread)
    # ──────────────────────────────────────────

    @_probe.decorate('ui.menu.bar_gpu_tick')
    def tick(self,
             screen_x: int, screen_y: int,
             strip_w: int, strip_h: int,
             snapshots: List[_ButtonSnapshot],
             color_fns: 'BarColorFns') -> None:
        """Drain the previous frame and (if state changed) submit a
        new compose. Cheap to call every tick — does no PIL work and
        early-outs when the snapshot signature is unchanged.
        """
        if self._destroyed or not snapshots:
            return
        if not self._ensure_window(strip_w, strip_h, screen_x, screen_y):
            return

        # 1) Drain previous frame and present it.
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

        # 2) Build dedup signature. Quantize size to 1/4 px and hover_t
        #    to 1/20 (matches SAOCircleButton._draw quantization so we
        #    submit one frame per visually-distinct state).
        sig_buttons = tuple(
            (round(s.size * 4.0) / 4.0,
             round(s.hover_t * 20.0) / 20.0,
             s.active,
             s.icon)
            for s in snapshots
        )
        sig = (int(strip_w), int(strip_h), len(snapshots), sig_buttons)
        with self._lock:
            if sig == self._last_sig:
                # Position can still drift even when visuals are static
                # (popup breathing). Update geometry only.
                if (screen_x, screen_y) != (self._last_geom[0], self._last_geom[1]) \
                        if self._last_geom else True:
                    pass  # presented frame above already moved it
                return
            self._last_sig = sig

        # 3) Submit a new compose. Capture state by value into the
        #    closure — never touch Tk on the worker.
        states = list(snapshots)
        slot = self._slot
        max_sz = self._max_size
        renderer = self._renderer
        cf = color_fns

        def compose(_now: float) -> Image.Image:
            img = Image.new('RGBA', (int(strip_w), int(strip_h)), (0, 0, 0, 0))
            for i, b in enumerate(states):
                size = max(1, int(round(b.size)))
                t = max(0.0, min(1.0, b.hover_t))
                if b.active:
                    border_color = cf.active_border
                    inner_fill = cf.active_bg
                    icon_color = cf.active_icon
                else:
                    border_color = cf.lerp(cf.border, cf.active_border, t)
                    inner_fill = cf.lerp(cf.bg, cf.hover_bg, t)
                    icon_color = cf.lerp(cf.icon, cf.hover_icon, t)
                sprite = renderer.render(
                    size, b.icon or '●',
                    border_color, inner_fill, icon_color, '#010101',
                )
                # Vertical strip: each button centered in its slot rect.
                slot_top = i * slot
                cx = (max_sz - size) // 2
                cy = slot_top + (slot - size) // 2
                img.alpha_composite(sprite, (cx, cy))
            return img

        try:
            self._render_worker.submit(
                compose, time.perf_counter(),
                0, int(screen_x), int(screen_y),
            )
        except Exception:
            pass


class BarColorFns:
    """Frozen color helpers + palette shared with SAOCircleButton.
    Captured once per painter so the worker closure doesn't need to
    import sao_theme (avoids circular import + Tk-touch surface)."""

    __slots__ = (
        'border', 'bg', 'icon',
        'active_border', 'active_bg', 'active_icon',
        'hover_bg', 'hover_icon',
        'lerp',
    )

    def __init__(self, palette: dict, lerp_fn: Callable[[str, str, float], str]):
        self.border = palette['border']
        self.bg = palette['bg']
        self.icon = palette['icon']
        self.active_border = palette['active_border']
        self.active_bg = palette['active_bg']
        self.active_icon = palette['active_icon']
        self.hover_bg = palette['hover_bg']
        self.hover_icon = palette['hover_icon']
        self.lerp = lerp_fn
