"""SAOPopUpMenu — GPU-native rewrite.

Lifecycle:
    open()  → creates the chroma-key Tk shell (for left_widget host) +
              the GpuOverlayWindow (menu/child/HUD), starts the tick.
    close() → fade out, destroy both, unregister scheduler.

Threading: every tk/glfw call happens on the Tk main thread (driven by
the existing GLFW pump installed on root via overlay_scheduler).

"""

from __future__ import annotations

import sys
import time
import tkinter as tk
from collections import deque
from typing import Callable, Deque, Dict, List, Optional, Tuple

import gpu_overlay_window as _gow
from overlay_scheduler import get_scheduler as _get_scheduler
from perf_probe import phase as _phase_trace

from .state import PopupState
from . import composer, menu_bar_layout, child_bar_layout, hud_layout
from .hit_test import HitTester, KIND_MENU_BTN, KIND_CHILD_ROW


_TRANSPARENT_KEY = '#010101'

# GLFW mouse button + action constants (avoid importing glfw module here)
_MOUSE_BUTTON_LEFT = 0
_ACTION_PRESS = 1
_ACTION_RELEASE = 0


class SAOPopUpMenu:
    """GPU-native popup menu. API matches the legacy Tk class."""

    def __init__(self, root: tk.Tk, icon_arr: List[Dict],
                 child_menus: Dict[str, List[Dict]],
                 username: str = 'Player',
                 description: str = 'Welcome to SAO world',
                 on_close: Optional[Callable] = None,
                 on_open: Optional[Callable] = None,
                 key_code: str = 'a',
                 slide_down: bool = True,
                 left_widget_factory: Optional[Callable] = None,
                 anchor_widget=None):
        if not _gow.glfw_supported():
            raise RuntimeError(
                'SAOPopUpMenu requires GLFW; gpu_overlay_window reports it is unavailable')

        self.root = root
        self.icon_arr = list(icon_arr)
        self.child_menus = dict(child_menus)
        self.username = username
        self.description = description
        self.on_close_callback = on_close
        self.on_open_callback = on_open
        self.key_code = key_code
        self.slide_down = slide_down
        self.left_widget_factory = left_widget_factory
        self.anchor_widget = anchor_widget

        self._state = PopupState()
        self._state.menu_items = list(icon_arr)

        self._gpu_win: Optional[_gow.GpuOverlayWindow] = None
        self._presenter: Optional[_gow.BgraPresenter] = None
        self._hit = HitTester()

        self._shell: Optional[tk.Toplevel] = None
        self._left_widget = None

        self._sched_ident = f'sao_popup_{id(self)}'
        self._sched_registered = False

        self._fade_target = 0.0
        self._fade_t0 = 0.0
        self._fading = False
        self._closing = False
        self._fade_duration = None
        self._last_tick_t = 0.0
        self._external_close_prepared = False
        self._skip_close_callback = False
        self._click_guard_resume: Optional[Callable[[], None]] = None
        # Toggle debounce: alt-key auto-repeat fires multiple
        # <Alt-KeyPress> events while the user is still pressing the
        # combo. Without a debounce, open() (which now does a slow
        # WGC pause_capture for the click guard) can be sandwiched by
        # a queued repeat-event that immediately fires close(). Result:
        # popup pops up and instantly closes. Ignore toggle requests
        # landing inside this debounce window.
        self._last_toggle_t: float = 0.0
        self._toggle_debounce_s: float = 0.35

        # Slide-down gesture state
        self._first_y = 0
        self._first_time = 0.0
        self._slide_threshold = 250
        self._slide_duration = 666
        self._throttle_until = 0.0

        # Outside-click watchdog
        self._focus_poll_job = None
        self._had_foreground_once = False
        self._last_raise_t = 0.0

        # Re-entrancy guard for GLFW mouse callbacks: any user cmd may tear
        # down this very GPU window (close menu / switch UI / quit). Defer
        # via after_idle and gate rapid clicks so we don't queue multiple
        # daemon threads (sao_sound) from one poll_events call which can
        # blow up the GIL with NULL tstate. See popup_glfw_callback_reentrancy.
        self._click_pending = False

        # CRITICAL: Tk's Tcl mainloop runs its own ``PeekMessage(NULL,...)``
        # message pump on Windows. It will pick up GLFW window messages
        # from the thread queue and ``DispatchMessage`` them to GLFW's
        # WndProc, which calls our ``_on_mouse_button`` cb. If the cb
        # touches ANY Tk API (``after_idle``, ``after``, widget ops) from
        # this re-entrant context, Tcl's interpreter state is mutated
        # mid-dispatch and the next Tcl checkpoint inside ``mainloop``
        # fast-fails with ``PyEval_RestoreThread: NULL tstate``.
        # Solution: cb only appends to a deque; a polling drainer on a
        # plain Tk ``after()`` schedule (always top-level Tk context)
        # consumes the queue and runs the actual handlers safely.
        self._click_queue: Deque[Tuple[str, int]] = deque()
        self._click_drain_job: Optional[str] = None

        # Public alias for legacy compat (sao_gui caches `_left_widget`)
        # exposed via property below.

    # ── public API ────────────────────────────────────────────────

    @property
    def visible(self) -> bool:
        return self._state.is_open

    @property
    def left_widget(self):
        return self._left_widget

    def bind_events(self) -> None:
        self.root.bind_all('<Alt-KeyPress>', self._on_alt_key, add='+')
        if self.slide_down:
            self.root.bind_all('<ButtonPress-1>', self._on_mouse_down, add='+')
            self.root.bind_all('<B1-Motion>', self._on_mouse_drag, add='+')

    def unbind_events(self) -> None:
        try:
            self.root.unbind_all('<Alt-KeyPress>')
            if self.slide_down:
                self.root.unbind_all('<ButtonPress-1>')
                self.root.unbind_all('<B1-Motion>')
        except Exception:
            pass

    def open(self) -> None:
        if self._state.is_open:
            return
        # Click guard (WGC pause_capture) is acquired inside
        # _create_window, AFTER is_open is set, so a held alt-key's
        # auto-repeat <Alt-KeyPress> event delivered while pause_capture
        # is blocking can't sandwich a close() before is_open flips.
        self._state.is_open = True
        self._closing = False
        self._external_close_prepared = False
        self._skip_close_callback = False
        self._had_foreground_once = False
        self._last_raise_t = 0.0
        self._fading = True
        self._fade_target = 1.0
        self._fade_duration = 0.45
        self._fade_t0 = time.monotonic()
        self._state.open_t0 = self._fade_t0
        self._state.row_anim_t0 = self._fade_t0
        self._last_tick_t = self._fade_t0
        self._state.fade_alpha = 0.0
        self._state.child_fade_t = 1.0
        self._state.child_phase = 'idle'
        self._state.pending_child_rows = []
        initial_btn = max(12.0, float(menu_bar_layout.SIZE) * 0.42)
        visible = menu_bar_layout.visible_count(self._state)
        self._state.btn_size = [initial_btn] * visible
        self._state.btn_hover_t = [0.0] * visible
        self._create_window()
        if self.on_open_callback:
            try:
                self.on_open_callback()
            except Exception:
                pass

    def close(self) -> None:
        if not self._state.is_open or self._closing:
            return
        # NOTE: do NOT release the click guard here; popup is still
        # visible during fade-out and any pending after_idle activate /
        # row callbacks must remain protected. Guard is released in
        # _destroy_window after the popup is actually torn down.
        self._closing = True
        self._fading = True
        self._fade_target = 0.0
        self._fade_duration = 0.30
        self._fade_t0 = time.monotonic()

    def toggle(self) -> None:
        # Debounce against alt-key auto-repeat (which would otherwise
        # open-then-close in rapid succession because pause_capture in
        # open() can stall the main thread for several ms).
        now = time.monotonic()
        if now - self._last_toggle_t < self._toggle_debounce_s:
            return
        self._last_toggle_t = now
        if self._state.is_open:
            self.close()
        else:
            self.open()

    def force_destroy_overlay(self, invoke_callback: bool = False) -> None:
        self._release_click_guard()
        self._closing = True
        self._destroy_window()
        if invoke_callback and self.on_close_callback:
            try:
                self.on_close_callback()
            except Exception:
                pass

    def prepare_external_fade(self) -> None:
        # Host exit animation handshake. Fade the GPU popup out in sync
        # with the global motion-blur overlay, but do not tear it down or
        # fire the normal close callback yet.
        if not self._state.is_open:
            return
        self._external_close_prepared = True
        self._skip_close_callback = True
        self._closing = True
        self._fading = True
        self._fade_target = 0.0
        self._fade_duration = 0.46
        self._fade_t0 = time.monotonic()
        if self._focus_poll_job is not None:
            try:
                self.root.after_cancel(self._focus_poll_job)
            except Exception:
                pass
            self._focus_poll_job = None

    def refresh_child_menus(self, menus: Dict[str, List[Dict]],
                            force: bool = False) -> None:
        self.child_menus = dict(menus)
        # If the active menu's items changed, rebuild rows
        idx = self._state.active_menu_idx
        if idx is not None and 0 <= idx < len(self._state.menu_items):
            name = self._state.menu_items[idx].get('name', '')
            self._state.child_rows = list(self.child_menus.get(name, []))
            self._reset_row_anim()
        elif force:
            self._state.child_rows = []
            self._reset_row_anim()
        if self._gpu_win is not None:
            self._gpu_win.request_redraw()

    def refresh_child_menu(self, name: str, items: List[Dict]) -> None:
        self.child_menus[name] = list(items)
        idx = self._state.active_menu_idx
        if (idx is not None and 0 <= idx < len(self._state.menu_items)
                and self._state.menu_items[idx].get('name') == name):
            self._state.child_rows = list(items)
            self._reset_row_anim()
            if self._gpu_win is not None:
                self._gpu_win.request_redraw()

    # ── internal: window lifecycle ────────────────────────────────

    def _create_window(self) -> None:
        # Acquire WGC click guard NOW (before any GLFW window create or
        # moderngl context init). is_open has already been set in open()
        # so a queued alt-key auto-repeat fires close() (which is then
        # honoured normally) instead of re-entering open().
        self._acquire_click_guard()
        # Start polled drainer for GLFW click events. Must be started
        # before any GLFW window can receive clicks.
        self._start_click_drainer()
        # Make sure SAO + CJK fonts are registered with the OS so the
        # Tk left-widget (and any PIL composer falling back to family
        # names) can resolve them on first paint.
        try:
            from sao_sound import load_sao_fonts as _lsf
            _lsf()
        except Exception:
            pass

        # Shell Tk Toplevel: hosts left_widget (if factory given) +
        # provides geometry parent and chroma-key for it.
        self._shell = tk.Toplevel(self.root)
        self._shell.overrideredirect(True)
        self._shell.attributes('-topmost', True)
        self._shell.configure(bg=_TRANSPARENT_KEY)
        try:
            self._shell.attributes('-transparentcolor', _TRANSPARENT_KEY)
        except Exception:
            pass

        # Build left widget if factory provided. Pack WITHOUT
        # fill/expand so the panel uses its own requested size; the
        # surrounding shell stays chroma-key transparent. The legacy
        # SAOPlayerPanel starts collapsed (0×0) and animates open via
        # set_active(True), called from _activate_menu below.
        if self.left_widget_factory is not None:
            try:
                self._left_widget = self.left_widget_factory(self._shell)
                self._left_widget.pack(anchor='nw')
            except Exception:
                self._left_widget = None

        # Compute popup positions.  Anchor next to anchor_widget if given,
        # else center on screen.
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        # Reserve GPU window size for the WORST-CASE child menu so that
        # switching between menus never clips rows or forces a resize
        # (resizes are jarring and visually break the compose layout).
        max_rows = 0
        for items in self.child_menus.values():
            try:
                if len(items) > max_rows:
                    max_rows = len(items)
            except Exception:
                continue
        self._reserved_rows = max_rows
        win_w, win_h = composer.window_size_reserved(self._state, max_rows)
        win_w = max(win_w, 200)
        win_h = max(win_h, 200)

        left_w = 240
        left_h = 280
        if self._left_widget is not None:
            # Reserve enough room for the fully-grown panel so its
            # set_active(True) animation has space to expand into.
            # Empty parts of the shell remain chroma-key transparent.
            target_w = int(getattr(self._left_widget, '_target_w', 0) or 0)
            top_h = int(getattr(self._left_widget, '_top_h', 0) or 0)
            bot_h = int(getattr(self._left_widget, '_bottom_h', 0) or 0)
            left_w = max(left_w, target_w or 0)
            left_h = max(left_h, (top_h + bot_h) or 0)

        margin = 16
        gap_left_to_gpu = 25
        total_w = left_w + gap_left_to_gpu + win_w
        total_h = max(left_h, win_h)

        aw = self.anchor_widget
        if aw is not None:
            try:
                ax = aw.winfo_rootx() + aw.winfo_width() - 8
                ay = aw.winfo_rooty() - 8
                # Position popup so its bottom-right corner sits at (ax, ay)
                base_x = max(margin, ax - total_w)
                base_y = max(margin, ay - total_h)
            except Exception:
                base_x = max(margin, (sw - total_w) // 2)
                base_y = max(margin, (sh - total_h) // 2)
        else:
            base_x = max(margin, (sw - total_w) // 2)
            base_y = max(margin, (sh - total_h) // 2)

        # Place left_widget shell
        if self._left_widget is not None:
            self._shell.geometry(f'{left_w}x{left_h}+{base_x}+{base_y + (total_h - left_h) // 2}')
        else:
            # No left widget — keep shell tiny and offscreen-ish (acts as
            # an anchor for Tk events only). Some platforms need a
            # non-zero size.
            self._shell.geometry(f'1x1+{-2}+{-2}')

        # GPU window position: to the right of the left_widget
        gpu_x = base_x + left_w + gap_left_to_gpu
        gpu_y = base_y + (total_h - win_h) // 2

        # Create GPU window
        pump = _gow.get_glfw_pump(self.root)
        self._presenter = _gow.BgraPresenter()
        self._gpu_win = _gow.GpuOverlayWindow(
            pump,
            w=win_w, h=win_h,
            x=gpu_x, y=gpu_y,
            render_fn=self._presenter.render,
            click_through=False,
            title='sao_popup_gpu',
        )
        self._gpu_win.set_input_callbacks(
            cursor_pos_fn=self._on_cursor_pos,
            cursor_leave_fn=self._on_cursor_leave,
            mouse_button_fn=self._on_mouse_button,
            scroll_fn=self._on_scroll_gpu,
        )
        self._gpu_win.show()

        # Register tick with overlay_scheduler so we get smooth animation
        try:
            _get_scheduler(self.root).register(
                self._sched_ident, self._tick, self._animating)
            self._sched_registered = True
        except Exception:
            self._sched_registered = False
            # Fall back to Tk after-loop
            self._fallback_tick()

        # Outside-click watchdog: poll foreground HWND every 100ms
        self._focus_poll_job = self.root.after(150, self._poll_foreground)

        # Prime first frame
        self._render_once()
        # Make sure we sit at the top of the topmost stack from the
        # start so existing combat overlays (DPS / boss-HP / skill-fx)
        # don't keep us beneath them.
        self._raise_to_top()

    def _destroy_window(self) -> None:
        # Stop click drainer first so no more queued clicks fire after
        # widgets are gone.
        self._stop_click_drainer()
        if self._sched_registered:
            try:
                _get_scheduler(self.root).unregister(self._sched_ident)
            except Exception:
                pass
            self._sched_registered = False
        if self._focus_poll_job is not None:
            try:
                self.root.after_cancel(self._focus_poll_job)
            except Exception:
                pass
            self._focus_poll_job = None
        if self._gpu_win is not None:
            try:
                self._gpu_win.destroy()
            except Exception:
                pass
            self._gpu_win = None
        if self._presenter is not None:
            try:
                self._presenter.release()
            except Exception:
                pass
            self._presenter = None
        if self._shell is not None:
            try:
                self._shell.destroy()
            except Exception:
                pass
            self._shell = None
        self._left_widget = None
        self._state.is_open = False
        self._state.fade_alpha = 0.0
        self._state.active_menu_idx = None
        self._state.hover_btn_idx = None
        self._state.hover_row_idx = None
        self._state.child_rows = []
        self._state.pending_child_rows = []
        self._state.btn_size = []
        self._state.btn_hover_t = []
        self._state.row_anim_w = []
        self._state.row_hover_t = []
        self._state.child_fade_t = 1.0
        self._state.child_phase = 'idle'
        self._fade_duration = None
        self._last_tick_t = 0.0
        self._external_close_prepared = False
        self._skip_close_callback = False
        # Popup fully torn down — safe to resume WGC capture.
        self._release_click_guard()

    # ── ticking + rendering ───────────────────────────────────────

    def _animating(self) -> bool:
        if not self._state.is_open and not self._closing:
            return False
        if self._fading:
            return True
        # HUD is always running while open
        return self._state.is_open

    def _tick(self, now: float) -> None:
        if self._gpu_win is None:
            return
        # overlay_scheduler drives jobs with time.time(), while this popup
        # uses time.monotonic() baselines for fade / child-row animation.
        # Mixing the two clocks collapses every animation to its end-state
        # in a single frame. Normalize everything onto monotonic time here.
        tick_now = time.monotonic()
        dt = min(0.10, max(0.0, (tick_now - self._last_tick_t) if self._last_tick_t else (1.0 / 60.0)))
        self._last_tick_t = tick_now
        # Fade
        if self._fading:
            elapsed = tick_now - self._fade_t0
            DUR = float(self._fade_duration or (0.45 if self._fade_target > 0 else 0.30))
            t = max(0.0, min(1.0, elapsed / DUR))
            if self._fade_target > 0:
                self._state.fade_alpha = t
            else:
                self._state.fade_alpha = 1.0 - t
            if t >= 1.0:
                self._fading = False
                self._fade_duration = None
                if self._fade_target <= 0:
                    if self._external_close_prepared:
                        self._closing = False
                        self._state.is_open = False
                        self._state.fade_alpha = 0.0
                        return
                    # Close finished
                    cb = None if self._skip_close_callback else self.on_close_callback
                    self._destroy_window()
                    if cb:
                        try:
                            cb()
                        except Exception:
                            pass
                    return
        self._advance_child_phase(dt)
        # Animations
        menu_bar_layout.advance_animation(self._state)
        child_bar_layout.advance_animation(self._state, tick_now)
        # HUD phase
        self._state.hud_phase = tick_now - self._state.open_t0
        # Periodically re-raise our GPU window to the top of the
        # HWND_TOPMOST stack. Other overlays (DPS / boss-HP / skill-fx)
        # created later are click-through topmost windows that would
        # otherwise sit above us in Z-order; while click-through means
        # the OS routes clicks down to us, GLFW input delivery can be
        # flaky in that configuration. Re-raising every ~250ms keeps
        # us at the top so clicks reach our window directly.
        if (tick_now - self._last_raise_t) > 0.25:
            self._last_raise_t = tick_now
            self._raise_to_top()
        self._render_once()

    def _fallback_tick(self) -> None:
        """Used if scheduler.register fails. Drives the tick via after()."""
        if not self._state.is_open and not self._closing:
            return
        try:
            self._tick(time.monotonic())
        finally:
            if self._state.is_open or self._closing:
                self.root.after(16, self._fallback_tick)

    def _render_once(self) -> None:
        if self._gpu_win is None or self._presenter is None:
            return
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
            rgba = composer.compose_rgba(
                self._state, self._state.hud_phase, sw, sh,
                reserved_rows=getattr(self, '_reserved_rows', 0))
            bgra = composer.to_premultiplied_bgra(rgba, self._state.fade_alpha)
            self._presenter.set_frame(bgra, rgba.width, rgba.height)
        except Exception:
            return
        # Update hit-test geometry after layouts settle
        self._hit.update(self._state)
        self._gpu_win.request_redraw()

    # ── input callbacks (called on Tk main thread by GLFW pump) ───

    def _on_cursor_pos(self, x: float, y: float) -> None:
        # Rapid mouse-move events: only update state. The 60Hz
        # scheduler tick will compose + request_redraw once per frame,
        # avoiding `after_cancel`+`after(0)` flooding from kick_redraw
        # which can starve Tk's event loop and crash with a GIL fault.
        if not self._state.is_open:
            return
        hit = self._hit.pick(x, y)
        if hit is None:
            self._state.hover_btn_idx = None
            self._state.hover_row_idx = None
        else:
            kind, idx = hit
            if kind == KIND_MENU_BTN:
                self._state.hover_btn_idx = idx
                self._state.hover_row_idx = None
            elif kind == KIND_CHILD_ROW:
                self._state.hover_row_idx = idx
                self._state.hover_btn_idx = None

    def _on_cursor_leave(self) -> None:
        self._state.hover_btn_idx = None
        self._state.hover_row_idx = None

    def _on_mouse_button(self, button: int, action: int, mods: int,
                         x: float, y: float) -> None:
        if button != _MOUSE_BUTTON_LEFT or action != _ACTION_PRESS:
            return
        if not self._state.is_open:
            return
        hit = self._hit.pick(x, y)
        if hit is None:
            return
        if self._click_pending:
            return
        kind, idx = hit
        # Re-entrance safety: this cb can fire from Tk's hidden Win32
        # message pump (PeekMessage(NULL)→DispatchMessage→GLFW WndProc).
        # Touching ANY Tk API here would corrupt Tcl state mid-dispatch
        # and crash the next mainloop iteration with a NULL tstate. So
        # we ONLY append to a thread-safe deque (deque.append + popleft
        # are atomic under the GIL) and let the polled drainer run on a
        # top-level Tk context.
        if kind == KIND_MENU_BTN:
            self._click_pending = True
            self._click_queue.append(('menu', int(idx)))
            _phase_trace('popup.click.enqueue', f'menu:{idx}')
        elif kind == KIND_CHILD_ROW:
            self._click_pending = True
            self._click_queue.append(('row', int(idx)))
            _phase_trace('popup.click.enqueue', f'row:{idx}')

    def _drain_click_queue(self) -> None:
        """Drain queued GLFW clicks. Always invoked from a top-level Tk
        ``after()`` callback so calling Tk APIs is safe here."""
        self._click_drain_job = None
        try:
            while self._click_queue:
                kind, idx = self._click_queue.popleft()
                _phase_trace('popup.click.drain', f'{kind}:{idx}')
                try:
                    if kind == 'menu':
                        self._fire_activate_menu(idx)
                    elif kind == 'row':
                        self._fire_invoke_row(idx)
                except Exception:
                    pass
        finally:
            # Reschedule while popup is open so we keep polling.
            if self._state.is_open and not self._closing:
                try:
                    self._click_drain_job = self.root.after(
                        16, self._drain_click_queue)
                except Exception:
                    self._click_drain_job = None

    def _start_click_drainer(self) -> None:
        if self._click_drain_job is not None:
            return
        try:
            self._click_drain_job = self.root.after(16, self._drain_click_queue)
        except Exception:
            self._click_drain_job = None

    def _stop_click_drainer(self) -> None:
        job = self._click_drain_job
        self._click_drain_job = None
        if job is not None:
            try:
                self.root.after_cancel(job)
            except Exception:
                pass
        self._click_queue.clear()


    def _acquire_click_guard(self) -> None:
        if self._click_guard_resume is not None:
            return
        try:
            from gpu_capture import pause_capture as _wgc_pause
            from gpu_capture import resume_capture as _wgc_resume
        except Exception:
            return
        try:
            _wgc_pause()
            self._click_guard_resume = _wgc_resume
            _phase_trace('popup.click.guard.on')
        except Exception:
            self._click_guard_resume = None

    def _release_click_guard(self) -> None:
        resume = self._click_guard_resume
        self._click_guard_resume = None
        if resume is None:
            return
        try:
            resume()
            _phase_trace('popup.click.guard.off')
        except Exception:
            pass

    def _fire_activate_menu(self, idx: int) -> None:
        _phase_trace('popup.activate.begin', f'idx={idx}')
        try:
            try:
                _phase_trace('popup.activate.cb', f'idx={idx}')
                self._activate_menu(idx)
            except Exception:
                pass
        finally:
            pass
        _phase_trace('popup.activate.end', f'idx={idx}')
        try:
            self.root.after(120, self._clear_click_pending)
        except Exception:
            self._click_pending = False

    def _fire_invoke_row(self, idx: int) -> None:
        _phase_trace('popup.row.begin', f'idx={idx}')
        try:
            try:
                _phase_trace('popup.row.cb', f'idx={idx}')
                self._invoke_row(idx)
            except Exception:
                pass
        finally:
            pass
        _phase_trace('popup.row.end', f'idx={idx}')
        try:
            self.root.after(120, self._clear_click_pending)
        except Exception:
            self._click_pending = False

    def _clear_click_pending(self) -> None:
        self._click_pending = False

    def _on_scroll_gpu(self, dx: float, dy: float) -> None:
        if not self._state.is_open or not self._state.menu_items:
            return
        if len(self._state.menu_items) <= menu_bar_layout.MAX_VISIBLE:
            return
        # Rotate the icon list (matches legacy SAOMenuBar._scroll_by_delta)
        if dy > 0:
            item = self._state.menu_items.pop()
            self._state.menu_items.insert(0, item)
        elif dy < 0:
            item = self._state.menu_items.pop(0)
            self._state.menu_items.append(item)
        else:
            return
        self._state.active_menu_idx = None
        self._state.child_rows = []
        self._state.pending_child_rows = []
        self._state.child_fade_t = 1.0
        self._state.child_phase = 'idle'
        self._state.row_anim_w = []
        self._state.row_hover_t = []

    def _activate_menu(self, idx: int) -> None:
        _phase_trace('popup.act.enter', f'idx={idx}')
        if idx < 0 or idx >= len(self._state.menu_items):
            _phase_trace('popup.act.bad_idx', f'idx={idx}')
            return
        item = self._state.menu_items[idx]
        if not item.get('can_active', True):
            _phase_trace('popup.act.no_active', f'idx={idx}')
            return
        _phase_trace('popup.act.sound.begin', f'idx={idx}')
        try:
            from sao_sound import play_sound as _ps
            _ps('click', volume=0.5)
        except Exception:
            pass
        _phase_trace('popup.act.sound.end', f'idx={idx}')
        if self._state.active_menu_idx == idx:
            _phase_trace('popup.act.deactivate', f'idx={idx}')
            self._state.active_menu_idx = None
            # Collapse the left player panel back to 0×0.
            lw = self._left_widget
            if lw is not None and hasattr(lw, 'set_active'):
                _phase_trace('popup.act.lw_off.begin')
                try:
                    lw.set_active(False)
                except Exception:
                    pass
                _phase_trace('popup.act.lw_off.end')
            _phase_trace('popup.act.transition.begin', 'rows=0')
            self._begin_child_transition([])
            _phase_trace('popup.act.transition.end')
        else:
            _phase_trace('popup.act.activate', f'idx={idx}')
            self._state.active_menu_idx = idx
            name = item.get('name', '')
            rows = self.child_menus.get(name, [])
            _phase_trace('popup.act.transition.begin', f'name={name} rows={len(rows)}')
            self._begin_child_transition(rows)
            _phase_trace('popup.act.transition.end')
            # Animate the left player panel in, mirroring legacy.
            lw = self._left_widget
            if lw is not None and hasattr(lw, 'set_active'):
                _phase_trace('popup.act.lw_on.begin')
                try:
                    lw.set_active(True)
                except Exception:
                    pass
                _phase_trace('popup.act.lw_on.end')
        _phase_trace('popup.act.exit', f'idx={idx}')

    def _invoke_row(self, idx: int) -> None:
        if idx < 0 or idx >= len(self._state.child_rows):
            return
        cmd = self._state.child_rows[idx].get('command')
        try:
            from sao_sound import play_sound as _ps
            _ps('click', volume=0.5)
        except Exception:
            pass
        if cmd:
            try:
                cmd()
            except Exception:
                pass

    def _advance_child_phase(self, dt: float) -> None:
        if self._state.child_phase == 'fadeout':
            self._state.child_fade_t = min(1.0, self._state.child_fade_t + dt / 0.16)
            if self._state.child_fade_t >= 0.999:
                self._state.child_rows = list(self._state.pending_child_rows)
                self._state.pending_child_rows = []
                if self._state.child_rows:
                    self._reset_row_anim()
                    self._state.child_fade_t = 1.0
                    self._state.child_phase = 'fadein'
                else:
                    self._state.row_anim_w = []
                    self._state.row_hover_t = []
                    self._state.hover_row_idx = None
                    self._state.child_phase = 'idle'
        elif self._state.child_phase == 'fadein':
            self._state.child_fade_t = max(0.0, self._state.child_fade_t - dt / 0.22)
            if self._state.child_fade_t <= 0.001:
                self._state.child_fade_t = 0.0
                self._state.child_phase = 'idle'

    def _begin_child_transition(self, rows: List[Dict]) -> None:
        next_rows = list(rows)
        if self._state.child_rows:
            self._state.pending_child_rows = next_rows
            self._state.child_phase = 'fadeout'
            return
        self._state.child_rows = next_rows
        self._state.pending_child_rows = []
        if next_rows:
            self._reset_row_anim()
            self._state.child_fade_t = 1.0
            self._state.child_phase = 'fadein'
        else:
            self._state.child_fade_t = 1.0
            self._state.child_phase = 'idle'

    def _reset_row_anim(self) -> None:
        n = len(self._state.child_rows)
        self._state.row_anim_w = [0] * n
        self._state.row_hover_t = [0.0] * n
        self._state.hover_row_idx = None
        self._state.row_anim_t0 = time.monotonic()

    # ── alt+a + slide gestures + outside click ────────────────────

    def _on_alt_key(self, e) -> None:
        if e.keysym.lower() == self.key_code.lower():
            self.toggle()

    def _on_mouse_down(self, e) -> None:
        self._first_y = e.y_root
        self._first_time = time.time() * 1000

    def _on_mouse_drag(self, e) -> None:
        if self._state.is_open:
            return
        dy = e.y_root - self._first_y
        dt = time.time() * 1000 - self._first_time
        if dy > self._slide_threshold and dt < self._slide_duration:
            now = time.monotonic()
            if now > self._throttle_until:
                self._throttle_until = now + 1.0
                self.open()

    def _poll_foreground(self) -> None:
        """Close the popup when the foreground HWND is none of our
        windows (root, shell, GPU)."""
        if not self._state.is_open or self._closing:
            return
        # Grace period: ignore foreground for first 600ms after open so
        # transient creation/init focus doesn't snap us shut.
        if (time.monotonic() - self._state.open_t0) < 0.6:
            try:
                self._focus_poll_job = self.root.after(150, self._poll_foreground)
            except Exception:
                self._focus_poll_job = None
            return
        try:
            ours = self._is_our_foreground()
            if ours:
                self._had_foreground_once = True
            elif self._had_foreground_once:
                # Only auto-close once we have actually held foreground
                # at least once — otherwise the popup was opened via a
                # global hotkey while another app (e.g. the game) was in
                # foreground, and we'd close ourselves immediately.
                self.close()
                return
        finally:
            try:
                self._focus_poll_job = self.root.after(150, self._poll_foreground)
            except Exception:
                self._focus_poll_job = None

    def _raise_to_top(self) -> None:
        """Bump our GPU window (and shell) to the top of the
        HWND_TOPMOST z-order without stealing focus."""
        if sys.platform != 'win32':
            return
        if self._gpu_win is None:
            return
        hwnd = int(getattr(self._gpu_win, '_hwnd', 0) or 0)
        if not hwnd:
            return
        try:
            import ctypes
            from ctypes import wintypes
            user32 = ctypes.windll.user32
            HWND_TOPMOST = -1
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            user32.SetWindowPos(
                wintypes.HWND(hwnd), wintypes.HWND(HWND_TOPMOST),
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        except Exception:
            pass

    def _is_our_foreground(self) -> bool:
        """Return True when the OS foreground window belongs to this
        process. Compares process IDs rather than HWNDs so that other
        toplevels of our own app (the main float window, dialogs, etc.)
        all count as 'ours' and don't trigger an auto-close."""
        if sys.platform != 'win32':
            return True  # only guard on Windows
        try:
            import ctypes
            user32 = ctypes.windll.user32
            kernel32 = ctypes.windll.kernel32
            fg = user32.GetForegroundWindow()
        except Exception:
            return True
        if not fg:
            # No foreground means alt-tab/desktop; treat as ours so we
            # don't ghost-close on a transient state change.
            return True
        try:
            our_pid = int(kernel32.GetCurrentProcessId())
            fg_pid = ctypes.c_ulong(0)
            user32.GetWindowThreadProcessId(
                ctypes.c_void_p(int(fg)), ctypes.byref(fg_pid))
            if int(fg_pid.value) == our_pid:
                return True
        except Exception:
            return True
        return False
