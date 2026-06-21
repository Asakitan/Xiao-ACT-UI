# -*- coding: utf-8 -*-
"""
SAOPlayerGUIFisheyeMixin — fifth mixin extracted from SAOPlayerGUI
(round 41 of the sao_gui split refactor). 9 methods, ~1362 lines.

Holds the persistent GPU fisheye overlay that runs behind the SAO
menu and floating panels. The biggest single method in the entire
SAOPlayerGUI class — _start_fisheye_overlay (1078 lines) — sets up
the background worker, GPU overlay window, hit-test layer, and
input z-order management for the fisheye effect.

Methods (in source order):
  * _fisheye_close_suppressed — short window after close to ignore re-open
  * _release_fisheye_input_zorder — Win32 SetWindowLongPtrW + SetWindowPos
    to make the GPU overlay transparent to input + non-topmost
  * _destroy_fisheye_hit_layer — destroys the transparent Tk hit layer
  * _start_fisheye_with_retry — startup with retry (menu may not be
    rendered yet on first toggle)
  * _any_panel_open — utility used by maybe_stop_fisheye to decide
    whether to keep the fisheye alive
  * _maybe_stop_fisheye — decision point: only stop if menu+panels all closed
  * _start_fisheye_overlay — the 1078-line setup block
  * _stop_fisheye_overlay — ordered worker/GPU shutdown
  * _run_fisheye_entry — entry-animation flow (menu_open + fisheye_in)

All methods are SAOPlayerGUI instance methods today; this mixin holds
the definitions. SAOPlayerGUI's __init__ still owns the relevant state
(_fisheye_ov, _fisheye_hit_layer, _fisheye_close_suppress_until,
_lift_loop_active, etc.) — the mixin only contains method bodies that
access self.X.
"""

from __future__ import annotations

import threading
import time
import tkinter as tk
from typing import Any, Optional

try:
    from render.gpu_capture import (
        capture_monitor_bgr_for_point, ensure_session, get_latest_bgr,
    )
except Exception:
    capture_monitor_bgr_for_point = None  # type: ignore[assignment]
    ensure_session = None  # type: ignore[assignment]
    get_latest_bgr = None  # type: ignore[assignment]

from utils.perf_probe import probe as _probe, gauge as _perf_gauge, phase as _phase_trace


class SAOPlayerGUIFisheyeMixin:
    """Mixin providing the persistent GPU fisheye overlay lifecycle.

    SAOPlayerGUI must initialise the following attributes in its
    __init__ (this is the existing contract — the mixin does not seed
    them):

      * self._fisheye_ov (overlay handle or None)
      * self._fisheye_hit_layer (Tk Toplevel or None)
      * self._fisheye_close_suppress_until (float)
      * self._lift_loop_active (bool)
      * self._destroyed (bool)
      * self._sao_menu, self._panels_hidden, per-panel handles (dynamic)
      * self.root (the Tk root)
    """

    @staticmethod
    def _detect_panel_attrs(obj):
        """Dynamically find all panel attribute names on the owner."""
        return tuple(k for k in vars(obj) if k.endswith('_panel') and k.startswith('_'))

    def _fisheye_close_suppressed(self) -> bool:
        try:
            return time.time() < float(getattr(self, '_fisheye_close_suppress_until', 0.0) or 0.0)
        except Exception:
            return False

    def _release_fisheye_input_zorder(self, ov=None):
        if ov is None:
            ov = getattr(self, '_fisheye_ov', None)
        gpu_win = getattr(ov, 'gpu_win', None) if ov is not None else None
        if gpu_win is None:
            return
        try:
            import ctypes as _ct
            _u32 = _ct.windll.user32
            _GWL_EXSTYLE = -20
            _WS_EX_TRANSPARENT = 0x00000020
            _HWND_NOTOPMOST = -2
            _SWP_NOMOVE = 0x0002
            _SWP_NOSIZE = 0x0001
            _SWP_NOACTIVATE = 0x0010
            _SWP_NOOWNERZORDER = 0x0200
            _hwnd = int(getattr(gpu_win, '_hwnd', 0) or 0)
            if _hwnd:
                _ex = _u32.GetWindowLongPtrW(_ct.c_void_p(_hwnd), _GWL_EXSTYLE)
                _u32.SetWindowLongPtrW(
                    _ct.c_void_p(_hwnd), _GWL_EXSTYLE,
                    _ex | _WS_EX_TRANSPARENT,
                )
                _u32.SetWindowPos(
                    _ct.c_void_p(_hwnd), _ct.c_void_p(_HWND_NOTOPMOST),
                    0, 0, 0, 0,
                    _SWP_NOMOVE | _SWP_NOSIZE
                    | _SWP_NOACTIVATE | _SWP_NOOWNERZORDER,
                )
        except Exception:
            pass

    def _restore_fisheye_exit_zorder(self, ov=None):
        """Make the GPU fisheye visible again for the exit fade.

        Panel mode demotes the GPU window so panel clicks work normally.
        When the user clicks the backdrop to dismiss the fisheye, briefly
        restore the render window to topmost but keep it click-through, then
        re-raise visible panels above it.
        """
        if ov is None:
            ov = getattr(self, '_fisheye_ov', None)
        gpu_win = getattr(ov, 'gpu_win', None) if ov is not None else None
        if gpu_win is None:
            return
        try:
            set_click_through = getattr(gpu_win, 'set_click_through', None)
            if callable(set_click_through):
                set_click_through(True)
            else:
                import ctypes as _ct
                _u32 = _ct.windll.user32
                _GWL_EXSTYLE = -20
                _WS_EX_LAYERED = 0x00080000
                _WS_EX_TRANSPARENT = 0x00000020
                _WS_EX_TOOLWINDOW = 0x00000080
                _WS_EX_NOACTIVATE = 0x08000000
                _HWND_TOPMOST = -1
                _SWP_NOMOVE = 0x0002
                _SWP_NOSIZE = 0x0001
                _SWP_NOACTIVATE = 0x0010
                _hwnd = int(getattr(gpu_win, '_hwnd', 0) or 0)
                if _hwnd:
                    _ex = _u32.GetWindowLongPtrW(_ct.c_void_p(_hwnd), _GWL_EXSTYLE)
                    _u32.SetWindowLongPtrW(
                        _ct.c_void_p(_hwnd), _GWL_EXSTYLE,
                        _ex | _WS_EX_LAYERED | _WS_EX_TRANSPARENT
                        | _WS_EX_TOOLWINDOW | _WS_EX_NOACTIVATE,
                    )
                    _u32.SetWindowPos(
                        _ct.c_void_p(_hwnd), _ct.c_void_p(_HWND_TOPMOST),
                        0, 0, 0, 0,
                        _SWP_NOMOVE | _SWP_NOSIZE | _SWP_NOACTIVATE,
                    )
        except Exception:
            pass
        raise_panel = getattr(self, '_raise_panel_window', None)
        if not callable(raise_panel):
            return
        try:
            panels = list(self._iter_fisheye_panels())
        except Exception:
            panels = []
        for panel in panels:
            try:
                if self._is_fisheye_panel_visible(panel):
                    raise_panel(panel)
            except Exception:
                pass

    def _destroy_fisheye_hit_layer(self):
        layer = getattr(self, '_fisheye_hit_layer', None)
        self._fisheye_hit_layer = None
        if layer is not None:
            try:
                if layer.winfo_exists():
                    layer.destroy()
            except Exception:
                pass

    def _set_fisheye_hit_layer_clickthrough(self, enabled: bool) -> None:
        layer = getattr(self, '_fisheye_hit_layer', None)
        if layer is None:
            return
        try:
            if not layer.winfo_exists():
                return
        except Exception:
            return
        try:
            import ctypes as _cth
            _u32h = _cth.windll.user32
            _GWL_EXSTYLE = -20
            _WS_EX_LAYERED = 0x00080000
            _WS_EX_TOOLWINDOW = 0x00000080
            _WS_EX_NOACTIVATE = 0x08000000
            _WS_EX_TRANSPARENT = 0x00000020
            _HWND_TOPMOST = -1
            _SWP_NOMOVE = 0x0002
            _SWP_NOSIZE = 0x0001
            _SWP_NOACTIVATE = 0x0010
            _SWP_NOOWNERZORDER = 0x0200
            layer.update_idletasks()
            hwnd = int(_u32h.GetParent(layer.winfo_id()) or layer.winfo_id())
            if not hwnd:
                return
            ex = _u32h.GetWindowLongPtrW(_cth.c_void_p(hwnd), _GWL_EXSTYLE)
            ex |= _WS_EX_LAYERED | _WS_EX_TOOLWINDOW | _WS_EX_NOACTIVATE
            if enabled:
                ex |= _WS_EX_TRANSPARENT
            else:
                ex &= ~_WS_EX_TRANSPARENT
            _u32h.SetWindowLongPtrW(_cth.c_void_p(hwnd), _GWL_EXSTYLE, ex)
            _u32h.SetWindowPos(
                _cth.c_void_p(hwnd), _cth.c_void_p(_HWND_TOPMOST),
                0, 0, 0, 0,
                _SWP_NOMOVE | _SWP_NOSIZE
                | _SWP_NOACTIVATE | _SWP_NOOWNERZORDER,
            )
        except Exception:
            pass
        if not enabled:
            self._raise_entity_surfaces_above_fisheye()

    def _raise_entity_surfaces_above_fisheye(self) -> None:
        """Keep the GPU menu/trigger above the transparent fisheye hit layer."""
        menu = getattr(self, '_sao_menu', None)
        try:
            if menu is not None and getattr(menu, 'visible', False):
                raise_to_top = getattr(menu, '_raise_to_top', None)
                if callable(raise_to_top):
                    raise_to_top()
        except Exception:
            pass
        try:
            sync_btn = getattr(self, '_sync_float_button_geometry', None)
            if callable(sync_btn):
                sync_btn(show=True)
        except Exception:
            pass

    def _prepare_fisheye_backdrop_for_panels(self) -> None:
        # Keep the transparent hit layer interactive while panels are open:
        # clicks on the backdrop should dismiss the fisheye, but panel windows
        # are raised above it by _raise_panel_window and remain clickable.
        self._set_fisheye_hit_layer_clickthrough(False)
        try:
            self._release_fisheye_input_zorder(getattr(self, '_fisheye_ov', None))
        except Exception:
            pass

    def _play_fisheye_backdrop_close_fx(self) -> None:
        """Play the same close flourish when dismissing panel-only fisheye."""
        try:
            from utils.sao_sound import play_sound as _play_sound
            _play_sound('menu_close')
        except Exception:
            pass
        try:
            motion_blur = getattr(self, '_play_motion_blur', None)
            if callable(motion_blur):
                motion_blur(closing=True)
        except Exception:
            pass

    def _request_fisheye_backdrop_close(self) -> None:
        self._sao_panel_transition_until = 0.0
        ov = self._fisheye_ov
        request_fadeout = getattr(ov, '_request_fadeout', None)
        if callable(request_fadeout):
            try:
                self._destroy_fisheye_hit_layer()
                self._play_fisheye_backdrop_close_fx()
                self._restore_fisheye_exit_zorder(ov)
                try:
                    request_fadeout(force=True)
                except TypeError:
                    request_fadeout()
                return
            except Exception:
                pass
        self._stop_fisheye_overlay()

    def _start_fisheye_with_retry(self, retries=5, delay=80):
        """带重试的鱼眼启动 — 首次进入时菜单可能还未完成渲染"""
        if self._destroyed:
            return
        if self._fisheye_close_suppressed():
            if retries > 0:
                try:
                    self.root.after(delay, lambda: self._start_fisheye_with_retry(retries - 1, delay))
                except Exception:
                    pass
            return
        menu_visible = bool(self._sao_menu is not None and self._sao_menu.visible)
        if self._fisheye_ov is not None:
            self._set_fisheye_hit_layer_clickthrough(False)
            self._raise_entity_surfaces_above_fisheye()
            return  # 已在运行
        if retries <= 0:
            return
        if menu_visible or self._any_panel_open():
            self._start_fisheye_overlay()
            self._raise_entity_surfaces_above_fisheye()
            for _delay in (80, 220, 420):
                try:
                    self.root.after(_delay, self._raise_entity_surfaces_above_fisheye)
                except Exception:
                    pass
        else:
            try:
                self.root.after(delay, lambda: self._start_fisheye_with_retry(retries - 1, delay))
            except Exception:
                pass

    _FISHEYE_EXCLUDE_PANELS = frozenset({'_process_selector_panel'})

    def _iter_fisheye_panels(self):
        seen = set()
        attrs = tuple(getattr(self, '_PLATFORM_PANEL_ATTRS', ()) or ())
        for attr in self._detect_panel_attrs(self) + tuple(attrs):
            if attr in seen or attr in self._FISHEYE_EXCLUDE_PANELS:
                continue
            seen.add(attr)
            yield getattr(self, attr, None)
        panels = getattr(self, '_plugin_detached_panels', None)
        if isinstance(panels, dict):
            for panel in list(panels.values()):
                yield panel

    def _is_fisheye_panel_visible(self, panel) -> bool:
        if panel is None:
            return False
        is_visible = getattr(panel, 'is_visible', None)
        if callable(is_visible):
            try:
                return bool(is_visible())
            except Exception:
                pass
        win = getattr(panel, '_win', panel)
        if win is None:
            return False
        try:
            if not win.winfo_exists():
                return False
        except Exception:
            return False
        try:
            return str(win.state()) != 'withdrawn'
        except Exception:
            return True

    def _any_panel_open(self):
        """检查是否有任何浮动面板处于打开且可见状态"""
        if getattr(self, '_panels_hidden', False):
            return False
        return any(self._is_fisheye_panel_visible(panel)
                   for panel in self._iter_fisheye_panels())

    def _maybe_stop_fisheye(self):
        """仅当 SAO 菜单和所有面板都关闭时才销毁鱼眼叠加层"""
        menu_visible = bool(
            self._sao_menu is not None and self._sao_menu.visible
            and not self._fisheye_close_suppressed())
        if menu_visible:
            self._set_fisheye_hit_layer_clickthrough(False)
            self._raise_entity_surfaces_above_fisheye()
            return
        if self._any_panel_open():
            self._prepare_fisheye_backdrop_for_panels()
            return
        try:
            pending_until = float(getattr(self, '_sao_panel_transition_until', 0.0) or 0.0)
        except Exception:
            pending_until = 0.0
        if pending_until and time.time() < pending_until:
            self._prepare_fisheye_backdrop_for_panels()
            try:
                self.root.after(180, self._maybe_stop_fisheye)
            except Exception:
                pass
            return
        ov = self._fisheye_ov
        # Play closing motion blur so the fisheye fade-out has visual
        # coverage — without it, the clear background bleeds through
        # as fisheye alpha decreases.  Delay the fadeout by 120 ms so
        # the motion blur's background thread has time to capture and
        # display before the fisheye starts losing opacity.
        try:
            motion_blur = getattr(self, '_play_motion_blur', None)
            if callable(motion_blur):
                motion_blur(closing=True)
        except Exception:
            pass
        request_fadeout = getattr(ov, '_request_fadeout', None)
        if callable(request_fadeout):
            try:
                _ov_ref = ov
                def _delayed_fadeout():
                    if self._fisheye_ov is _ov_ref:
                        request_fadeout()
                self.root.after(120, _delayed_fadeout)
                return
            except Exception:
                pass
        self._stop_fisheye_overlay()

    @_probe.decorate('ui.fisheye.start')
    def _start_fisheye_overlay(self):
        if self._fisheye_close_suppressed():
            return
        self._stop_fisheye_overlay()
        if not (self._sao_menu is not None and self._sao_menu.visible) and not self._any_panel_open():
            return
        # Process selector is mutually exclusive with fisheye
        ps = getattr(self, '_process_selector_panel', None)
        if ps is not None:
            try:
                ps.hide()
            except Exception:
                pass
        self._lift_loop_active = False

        try:
            from PIL import ImageGrab, Image
        except ImportError:
            return

        try:
            from render import gpu_overlay_window as _gow
        except Exception:
            _gow = None  # type: ignore[assignment]
        if _gow is None or not _gow.glfw_supported():
            # GPU path unavailable; bail (legacy Tk path removed for
            # the user-visible CPU win — the menu still works without
            # a fisheye backdrop).
            return

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        hw, hh = int(sw * 0.85), int(sh * 0.85)   # 85% 分辨率 (清晰度提升)

        # Fisheye is render-only. A separate transparent Tk hit layer owns
        # backdrop clicks, so the GPU window never steals mouse focus/z-order.
        _backdrop_drag = {
            'pressed': False, 'in_panel': False,
            'cursor_x': 0, 'cursor_y': 0, 'last_y': 0,
            'close_candidate': False,
        }
        _DRAG_ROW_PX = 40

        def _cursor_in_scroll_panel(x, y):
            for attr in self._detect_panel_attrs(self):
                panel = getattr(self, attr, None)
                if panel is None or not hasattr(panel, '_on_mousewheel'):
                    continue
                try:
                    if not panel.winfo_exists() or not panel.winfo_ismapped():
                        continue
                    px = int(panel.winfo_rootx())
                    py = int(panel.winfo_rooty())
                    pw = int(getattr(panel, 'PANEL_W', 0) or panel.winfo_width())
                    ph = int(getattr(panel, 'PANEL_H', 0) or panel.winfo_height())
                    if px <= x < px + pw and py <= y < py + ph:
                        return True, panel
                except Exception:
                    continue
            return False, None

        def _scroll_panel_by_rows(panel, rows):
            if not rows or panel is None:
                return
            try:
                total = len(getattr(panel, '_rows_data', ()) or ())
                visible = int(getattr(panel, '_visible_row_count', 0) or 0)
                max_first = max(0, total - visible)
                old_first = int(getattr(panel, '_first_visible_row', 0) or 0)
                new_first = max(0, min(max_first, old_first + int(rows)))
                if new_first != old_first:
                    panel._first_visible_row = new_first
                    panel._queue_gpu_paint(16)
            except Exception:
                pass

        def _backdrop_cursor_pos(x, y):
            _backdrop_drag['cursor_x'] = int(x)
            _backdrop_drag['cursor_y'] = int(y)
            if not _backdrop_drag['pressed'] or not _backdrop_drag['in_panel']:
                return
            _inside, panel = _cursor_in_scroll_panel(
                int(_backdrop_drag.get('cursor_x', 0)),
                int(_backdrop_drag.get('cursor_y', 0)),
            )
            if panel is None:
                return
            dy = int(y) - int(_backdrop_drag['last_y'])
            # Drag down → list scrolls up (touch-style). Convert to row delta.
            rows = -(dy // _DRAG_ROW_PX) if dy < 0 else -((dy + _DRAG_ROW_PX // 2) // _DRAG_ROW_PX)
            if rows == 0:
                return
            _scroll_panel_by_rows(panel, rows)
            _backdrop_drag['last_y'] = int(_backdrop_drag['last_y']) - rows * _DRAG_ROW_PX

        def _backdrop_scroll(dx, dy):
            cx = int(_backdrop_drag.get('cursor_x', 0))
            cy = int(_backdrop_drag.get('cursor_y', 0))
            inside, panel = _cursor_in_scroll_panel(cx, cy)
            if not inside or panel is None:
                return
            # GLFW yoffset: +up / -down. Tk wheel delta: +120 per notch up.
            delta = int(120 * float(dy))
            if delta == 0:
                delta = 120 if dy > 0 else (-120 if dy < 0 else 0)
            if delta == 0:
                return

            class _Evt:
                pass
            evt = _Evt()
            evt.x_root = cx
            evt.y_root = cy
            evt.delta = delta
            evt.num = 0
            try:
                panel._on_mousewheel(evt)
            except Exception:
                pass

        def _dispatch_sao_menu_click(x, y):
            menu = getattr(self, '_sao_menu', None)
            if menu is None or not bool(getattr(menu, 'visible', False)):
                return False
            menu_bar = getattr(menu, '_menu_bar', None)
            dispatch = getattr(menu_bar, 'dispatch_root_click', None)
            if not callable(dispatch):
                return False
            try:
                return bool(dispatch(int(x), int(y)))
            except Exception:
                return False

        def _close_sao_menu_from_backdrop(button=None, action=None, *_args):
            if button != 0:
                return
            x = int(_backdrop_drag.get('cursor_x', 0))
            y = int(_backdrop_drag.get('cursor_y', 0))
            inside, _ = _cursor_in_scroll_panel(x, y)
            if action == 1:  # press
                _backdrop_drag['pressed'] = True
                _backdrop_drag['in_panel'] = bool(inside)
                _backdrop_drag['last_y'] = y
                _backdrop_drag['close_candidate'] = not bool(inside)
                if inside:
                    return  # Reserve the click for drag scrolling
                return
            elif action == 0:  # release
                was_in_panel = bool(_backdrop_drag.get('in_panel', False))
                close_candidate = bool(_backdrop_drag.get('close_candidate', False))
                _backdrop_drag['pressed'] = False
                _backdrop_drag['in_panel'] = False
                _backdrop_drag['close_candidate'] = False
                if was_in_panel:
                    return  # Drag finished — do not close
                if _dispatch_sao_menu_click(x, y):
                    return
                if not close_candidate or inside:
                    return
            else:
                return
            try:
                menu_visible = bool(
                    self._sao_menu is not None and self._sao_menu.visible)
                if menu_visible:
                    self._close_sao_menu_from_background()
                elif self._any_panel_open():
                    self._request_fisheye_backdrop_close()
            except Exception:
                pass

        def _create_fisheye_hit_layer():
            self._destroy_fisheye_hit_layer()
            try:
                layer = tk.Toplevel(self.root)
                layer.overrideredirect(True)
                layer.attributes('-topmost', True)
                layer.attributes('-alpha', 0.01)
                layer.configure(bg='black', cursor='arrow')
                layer.geometry(f'{sw}x{sh}+0+0')
                layer.update_idletasks()
                try:
                    import ctypes as _cth
                    _u32h = _cth.windll.user32
                    _GWL_EXSTYLE = -20
                    _WS_EX_LAYERED = 0x00080000
                    _WS_EX_TOOLWINDOW = 0x00000080
                    _WS_EX_NOACTIVATE = 0x08000000
                    _HWND_TOPMOST = -1
                    _SWP_NOMOVE = 0x0002
                    _SWP_NOSIZE = 0x0001
                    _SWP_NOACTIVATE = 0x0010
                    _SWP_NOOWNERZORDER = 0x0200
                    hwnd = int(_u32h.GetParent(layer.winfo_id()) or layer.winfo_id())
                    ex = _u32h.GetWindowLongPtrW(_cth.c_void_p(hwnd), _GWL_EXSTYLE)
                    _u32h.SetWindowLongPtrW(
                        _cth.c_void_p(hwnd), _GWL_EXSTYLE,
                        ex | _WS_EX_LAYERED | _WS_EX_TOOLWINDOW | _WS_EX_NOACTIVATE,
                    )
                    _u32h.SetWindowPos(
                        _cth.c_void_p(hwnd), _cth.c_void_p(_HWND_TOPMOST),
                        0, 0, 0, 0,
                        _SWP_NOMOVE | _SWP_NOSIZE
                        | _SWP_NOACTIVATE | _SWP_NOOWNERZORDER,
                    )
                except Exception:
                    pass

                def _layer_pos(event):
                    try:
                        _backdrop_drag['cursor_x'] = int(event.x_root)
                        _backdrop_drag['cursor_y'] = int(event.y_root)
                    except Exception:
                        pass

                def _layer_press(event):
                    _layer_pos(event)
                    x = int(_backdrop_drag.get('cursor_x', 0))
                    y = int(_backdrop_drag.get('cursor_y', 0))
                    inside, _ = _cursor_in_scroll_panel(x, y)
                    _backdrop_drag['pressed'] = True
                    _backdrop_drag['in_panel'] = bool(inside)
                    _backdrop_drag['last_y'] = y
                    _backdrop_drag['close_candidate'] = not bool(inside)
                    return 'break'

                def _layer_release(event):
                    _layer_pos(event)
                    _close_sao_menu_from_backdrop(0, 0)
                    return 'break'

                def _layer_motion(event):
                    _layer_pos(event)
                    _backdrop_cursor_pos(event.x_root, event.y_root)
                    return 'break'

                def _layer_wheel(event):
                    _layer_pos(event)
                    delta = getattr(event, 'delta', 0) or 0
                    dy = 1.0 if delta > 0 else (-1.0 if delta < 0 else 0.0)
                    _backdrop_scroll(0.0, dy)
                    return 'break'

                layer.bind('<ButtonPress-1>', _layer_press)
                layer.bind('<ButtonRelease-1>', _layer_release)
                layer.bind('<B1-Motion>', _layer_motion)
                layer.bind('<Motion>', _layer_pos)
                layer.bind('<MouseWheel>', _layer_wheel)
                layer.bind('<Button-4>', lambda e: (_layer_pos(e), _backdrop_scroll(0.0, 1.0), 'break')[-1])
                layer.bind('<Button-5>', lambda e: (_layer_pos(e), _backdrop_scroll(0.0, -1.0), 'break')[-1])
                self._fisheye_hit_layer = layer
                try:
                    self._set_fisheye_hit_layer_clickthrough(False)
                except Exception:
                    pass
                try:
                    menu = getattr(self, '_sao_menu', None)
                    raise_to_top = getattr(menu, '_raise_to_top', None)
                    if callable(raise_to_top):
                        raise_to_top()
                except Exception:
                    pass
                try:
                    self._raise_entity_surfaces_above_fisheye()
                except Exception:
                    pass
            except Exception:
                self._fisheye_hit_layer = None

        # ── 创建 GPU 叠加窗口 (interactive backdrop, 自然位于 topmost UI 下方) ──
        try:
            pump = _gow.get_glfw_pump(self.root)

            class _FisheyeTexturePresenter:
                gpu_distorts = True

                def __init__(self):
                    self._prog = None
                    self._vbo = None
                    self._vao = None
                    self._tex = None
                    self._tex_w = 0
                    self._tex_h = 0
                    self._rgb = None
                    self._w = 0
                    self._h = 0
                    # 帧以 (bytes, w, h, seq) 单元组交接, 渲染线程不会读到
                    # 半更新组合; 单调 seq 替代 id() 身份比较 (释放后的
                    # bytes 地址被复用时 id 碰撞会静默跳过真实新帧)。
                    self._frame_snap = None
                    self._frame_seq = 0
                    self._uploaded_seq = -1
                    self._alpha = 1.0
                    self._fade_active = False
                    self._fade_t0 = 0.0
                    self._fade_dur = 0.0
                    self._fade_from = 1.0
                    self._fade_to = 1.0
                    self._fade_done_cb = None

                def set_frame(self, rgb_bytes, w, h):
                    if rgb_bytes is not self._rgb:
                        self._frame_seq += 1
                    self._rgb = rgb_bytes
                    self._w = int(w)
                    self._h = int(h)
                    self._frame_snap = (rgb_bytes, int(w), int(h), self._frame_seq)

                def set_alpha(self, alpha):
                    self._fade_active = False
                    self._alpha = max(0.0, min(1.0, float(alpha)))

                def start_fade(self, target_alpha, duration_s, on_done=None):
                    self._fade_from = float(self._alpha)
                    self._fade_to = max(0.0, min(1.0, float(target_alpha)))
                    self._fade_dur = max(0.001, float(duration_s))
                    self._fade_t0 = time.perf_counter()
                    self._fade_active = True
                    self._fade_done_cb = on_done

                def is_fading(self):
                    return bool(self._fade_active)

                def render(self, ctx, t):
                    if self._fade_active:
                        elapsed = time.perf_counter() - self._fade_t0
                        if elapsed >= self._fade_dur:
                            self._alpha = self._fade_to
                            self._fade_active = False
                            cb = self._fade_done_cb
                            self._fade_done_cb = None
                            if cb is not None:
                                try:
                                    cb()
                                except Exception:
                                    pass
                        else:
                            k = elapsed / self._fade_dur
                            self._alpha = self._fade_from + (self._fade_to - self._fade_from) * k
                    if self._prog is None:
                        import numpy as _np
                        self._prog = ctx.program(
                            vertex_shader='''
                                #version 330
                                in vec2 in_pos;
                                out vec2 v_uv;
                                void main() {
                                    v_uv = vec2(in_pos.x * 0.5 + 0.5,
                                                1.0 - (in_pos.y * 0.5 + 0.5));
                                    gl_Position = vec4(in_pos, 0.0, 1.0);
                                }
                            ''',
                            fragment_shader='''
                                #version 330
                                uniform sampler2D u_tex;
                                uniform float u_alpha;
                                uniform float u_time;
                                in vec2 v_uv;
                                out vec4 fragColor;

                                float line_band(float y, float center) {
                                    return 1.0 - smoothstep(0.0, 0.006, abs(y - center));
                                }

                                void main() {
                                    vec2 c = v_uv - 0.5;
                                    float r2 = dot(c, c);
                                    float open = clamp(u_alpha, 0.0, 1.0);
                                    float edge = 1.0 - open;
                                    float pulse = 0.5 + 0.5 * sin(u_time * 1.35);
                                    float strength = 0.44 + 0.18 * edge + 0.055 * pulse;
                                    vec2 drift = vec2(0.006 * sin(u_time * 0.62),
                                                      0.004 * cos(u_time * 0.51));
                                    vec2 uv = v_uv + c * strength * r2 + drift * (0.25 + edge);
                                    vec2 dir = normalize(c + vec2(0.0001)) * (0.006 + r2 * 0.026 + edge * 0.012);
                                    vec3 b0 = texture(u_tex, uv - dir * 2.0).rgb;
                                    vec3 b1 = texture(u_tex, uv - dir).rgb;
                                    vec3 b2 = texture(u_tex, uv).rgb;
                                    vec3 b3 = texture(u_tex, uv + dir).rgb;
                                    vec3 b4 = texture(u_tex, uv + dir * 2.0).rgb;
                                    vec3 blur = b0 * 0.10 + b1 * 0.20 + b2 * 0.36 + b3 * 0.22 + b4 * 0.12;
                                    float ca = 0.0015 + r2 * 0.004;
                                    vec3 col = mix(blur, vec3(
                                        texture(u_tex, uv + vec2(ca, 0.0)).r,
                                        texture(u_tex, uv).g,
                                        texture(u_tex, uv - vec2(ca, 0.0)).b
                                    ), 0.50);
                                    float scan = 0.92 + 0.08 * sin((v_uv.y + u_time * 0.055) * 980.0);
                                    float vignette = 0.86 - r2 * (0.38 + 0.28 * edge) + 0.055 * sin(u_time * 0.45);
                                    col *= scan * vignette;
                                    col = mix(col, vec3(0.0), 0.34 + 0.18 * edge);

                                    float radial = length(c);
                                    float ring_r = mix(0.18, 0.72, open);
                                    float ring = 1.0 - smoothstep(0.0, 0.026, abs(radial - ring_r));
                                    float ring2 = 1.0 - smoothstep(0.0, 0.016, abs(radial - mod(u_time * 0.13, 0.82)));
                                    col += vec3(0.10, 0.42, 0.50) * ring * (0.12 + 0.26 * edge);
                                    col += vec3(0.28, 0.20, 0.06) * ring2 * 0.05;

                                    float hud = 0.0;
                                    hud += line_band(v_uv.y, 0.08) * step(0.05, v_uv.x) * step(v_uv.x, 0.95);
                                    hud += line_band(v_uv.y, 0.15) * step(0.05, v_uv.x) * step(v_uv.x, 0.95);
                                    hud += line_band(v_uv.y, 0.85) * step(0.05, v_uv.x) * step(v_uv.x, 0.95);
                                    hud += line_band(v_uv.y, 0.92) * step(0.05, v_uv.x) * step(v_uv.x, 0.95);
                                    hud += (1.0 - smoothstep(0.0, 0.006, abs(v_uv.x - 0.5)))
                                           * step(0.42, v_uv.y) * step(v_uv.y, 0.47);
                                    hud += (1.0 - smoothstep(0.0, 0.006, abs(v_uv.y - 0.5)))
                                           * step(0.42, v_uv.x) * step(v_uv.x, 0.47);
                                    col += vec3(0.18, 0.48, 0.56) * min(hud, 1.0) * (0.16 + 0.08 * pulse);
                                    fragColor = vec4(col * u_alpha, u_alpha);
                                }
                            ''')
                        self._prog['u_tex'].value = 0
                        quad = _np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype='f4')
                        self._vbo = ctx.buffer(quad.tobytes())
                        self._vao = ctx.vertex_array(
                            self._prog, [(self._vbo, '2f', 'in_pos')])
                    try:
                        self._prog['u_alpha'].value = float(self._alpha)
                        self._prog['u_time'].value = float(t)
                    except Exception:
                        pass
                    snap = self._frame_snap
                    rgb, fw, fh, seq = snap if snap is not None else (None, 0, 0, -1)
                    if rgb is not None and fw > 0 and fh > 0:
                        if self._tex is None or self._tex_w != fw or self._tex_h != fh:
                            if self._tex is not None:
                                try:
                                    self._tex.release()
                                except Exception:
                                    pass
                            self._tex = ctx.texture((fw, fh), 3, rgb)
                            self._tex.filter = (_gow._moderngl.LINEAR, _gow._moderngl.LINEAR)
                            self._tex_w = fw
                            self._tex_h = fh
                            self._uploaded_seq = seq
                        elif seq != self._uploaded_seq:
                            self._tex.write(rgb)
                            self._uploaded_seq = seq
                    if self._tex is not None:
                        self._tex.use(location=0)
                        self._vao.render(_gow._moderngl.TRIANGLE_STRIP)

                def release(self):
                    for name in ('_tex', '_vao', '_vbo', '_prog'):
                        obj = getattr(self, name, None)
                        if obj is not None:
                            try:
                                obj.release()
                            except Exception:
                                pass
                            setattr(self, name, None)
                    self._rgb = None
                    self._frame_snap = None

            presenter = _FisheyeTexturePresenter()
            gpu_win = _gow.GpuOverlayWindow(
                pump,
                w=int(sw), h=int(sh),
                x=0, y=0,
                render_fn=presenter.render,
                click_through=True,
                title='sao_fisheye_gpu',
            )
            gpu_win.show()
            try:
                import ctypes as _ct0
                _u32a = _ct0.windll.user32
                _GWL_EXSTYLE = -20
                _WS_EX_LAYERED = 0x00080000
                _WS_EX_TOOLWINDOW = 0x00000080
                _WS_EX_NOACTIVATE = 0x08000000
                _WS_EX_TRANSPARENT = 0x00000020
                _LWA_ALPHA = 0x00000002
                _SWP_NOMOVE = 0x0002
                _SWP_NOSIZE = 0x0001
                _SWP_NOACTIVATE = 0x0010
                _hwnd_i = int(getattr(gpu_win, '_hwnd', 0) or 0)
                if _hwnd_i:
                    _cur = _u32a.GetWindowLongPtrW(_ct0.c_void_p(_hwnd_i), _GWL_EXSTYLE)
                    _new = (_cur | _WS_EX_LAYERED | _WS_EX_TOOLWINDOW
                        | _WS_EX_NOACTIVATE | _WS_EX_TRANSPARENT)
                    _u32a.SetWindowLongPtrW(_ct0.c_void_p(_hwnd_i), _GWL_EXSTYLE, _new)
                    _u32a.SetLayeredWindowAttributes(_ct0.c_void_p(_hwnd_i), 0, 255, _LWA_ALPHA)
                    _u32a.SetWindowPos(
                        _ct0.c_void_p(_hwnd_i), _ct0.c_void_p(-2),
                        0, 0, 0, 0,
                        _SWP_NOMOVE | _SWP_NOSIZE | _SWP_NOACTIVATE,
                    )
            except Exception:
                pass
        except Exception:
            return

        # 用一个轻量对象承载状态 (兼容 _stop 通过 _running_ref 关闭)
        class _FisheyeHandle:
            __slots__ = ('gpu_win', 'presenter', '_running_ref', '_worker_thread',
                         '_request_fadeout')
        ov = _FisheyeHandle()
        ov.gpu_win = gpu_win
        ov.presenter = presenter
        ov._worker_thread = None
        ov._request_fadeout = None
        self._fisheye_ov = ov
        _create_fisheye_hit_layer()

        _fisheye_capture_excluded = [False]

        def _set_fisheye_capture_excluded(exclude: bool):
            _hwnd = int(getattr(gpu_win, '_hwnd', 0) or 0)
            if not _hwnd:
                return
            ok = False
            try:
                from mem_probe._dc import apply as _dc_apply, remove as _dc_remove
                ok = _dc_apply(_hwnd) if exclude else _dc_remove(_hwnd)
            except Exception:
                pass
            if not ok:
                try:
                    import ctypes as _ct
                    _u32 = _ct.windll.user32
                    _u32.SetWindowDisplayAffinity.argtypes = [_ct.c_void_p, _ct.c_uint]
                    _u32.SetWindowDisplayAffinity.restype = _ct.c_int
                    flag = 0x00000011 if exclude else 0x00000000
                    ok = bool(_u32.SetWindowDisplayAffinity(_ct.c_void_p(_hwnd), flag))
                except Exception:
                    pass
            _fisheye_capture_excluded[0] = ok and exclude

        # v2.3.x+: 把鱼眼 GPU 窗口从 HWND_TOPMOST 栈降级 (有限次)。
        # GpuOverlayWindow 默认是 WS_EX_TOPMOST，并且在首次实际渲染时
        # 会调用 _show_no_activate(HWND_TOPMOST) 再次提权;同时 GLFW
        # set_window_size 等调用也可能恢复 topmost。鱼眼若停留在
        # topmost 栈，会盖住 SAO overlay 或导致点击穿透。
        # v2.3.10: 改为有限 8 次 (~2s) demote，避免无限 after(250)
        # 回调堆积导致主线程阻塞和鱼眼启用时间变长。
        try:
            import ctypes as _ct2
            _u32b = _ct2.windll.user32
            _HWND_NOTOPMOST = -2
            _SWP_NOMOVE = 0x0002
            _SWP_NOSIZE = 0x0001
            _SWP_NOACTIVATE = 0x0010
            _SWP_NOOWNERZORDER = 0x0200
            _demote_count = [12]  # finite demotions across slow first-frame drivers

            def _demote_fisheye():
                if self._fisheye_ov is None or _demote_count[0] <= 0:
                    return
                try:
                    _hwnd_d = int(getattr(gpu_win, '_hwnd', 0) or 0)
                    if _hwnd_d:
                        _u32b.SetWindowPos(
                            _ct2.c_void_p(_hwnd_d),
                            _ct2.c_void_p(_HWND_NOTOPMOST),
                            0, 0, 0, 0,
                            _SWP_NOMOVE | _SWP_NOSIZE
                            | _SWP_NOACTIVATE | _SWP_NOOWNERZORDER,
                        )
                except Exception:
                    pass
                _demote_count[0] -= 1
                if _demote_count[0] > 0:
                    try:
                        self.root.after(250, _demote_fisheye)
                    except Exception:
                        pass
                try:
                    self._raise_entity_surfaces_above_fisheye()
                except Exception:
                    pass

            # 第一次延迟 50ms,等 GLFW 完成首次 _show_no_activate 之后再降级。
            self.root.after(50, _demote_fisheye)
        except Exception:
            pass

        # ── Pump-driven fade + low-rate state monitor ──
        # v2.3.13: fade alpha is now driven by BgraPresenter.start_fade()
        # which animates inside the GLFW pump's render path (60 Hz, real
        # perf_counter time). Tk after-queue starvation no longer affects
        # fade smoothness. The remaining after() loop only:
        #   - pushes new frames from worker (dedup'd)
        #   - watches for menu close → triggers fade-out
        # so it can run at a leisurely 32 ms.
        _FADEIN_DUR = 0.5        # 500ms fade-in
        _FADEOUT_DUR = 0.4       # 400ms fade-out
        _running = [True]
        _latest_frame = [None]   # (frame_id, bgra_bytes, w, h) tuple
        _last_pushed_frame_id = [0]
        _state = ['init']        # init → fadein → active → fadeout → destroy
        _fade_started = [False]
        _fadeout_started = [False]
        _stop_requested = [False]
        _force_fadeout = [False]
        _frame_seq = [0]
        ov._running_ref = _running

        def _request_fadeout(force=False):
            if self._fisheye_ov is not ov:
                return
            _stop_requested[0] = True
            _force_fadeout[0] = bool(force)
            try:
                gpu_win.request_redraw()
            except Exception:
                pass

        ov._request_fadeout = _request_fadeout

        def _on_fadeout_done():
            def _stop_if_current(_expected=ov):
                if self._fisheye_ov is _expected:
                    self._stop_fisheye_overlay(wait=True, expected=_expected)

            try:
                pump = getattr(gpu_win, '_pump', None)
                post_to_tk = getattr(pump, 'post_to_tk', None)
                if callable(post_to_tk):
                    post_to_tk(_stop_if_current)
                else:
                    _stop_if_current()
            except Exception:
                pass

        def _tick():
            _tick_t0 = time.perf_counter()
            if self._fisheye_ov is not ov:
                return
            s = _state[0]

            # Worker thread now pushes frames directly to the presenter
            # at full rate (~60 Hz) bypassing Tk scheduling latency.
            # _tick only drives the state machine and fade control.

            # State machine — kicks off pump-driven fades.
            try:
                _actual_should_run = bool(
                    (self._sao_menu is not None and self._sao_menu.visible)
                    or self._any_panel_open())
            except Exception:
                _actual_should_run = False
            if self._fisheye_close_suppressed():
                _actual_should_run = False
            if _actual_should_run and not _force_fadeout[0]:
                _stop_requested[0] = False
            _fisheye_should_run = bool(_actual_should_run and not _stop_requested[0])

            if s == 'init':
                if _latest_frame[0] is not None:
                    try:
                        presenter.set_alpha(0.0)
                        presenter.start_fade(1.0, _FADEIN_DUR)
                        _fade_started[0] = True
                        gpu_win.request_redraw()
                    except Exception:
                        pass
                    _state[0] = 'fadein'
                elif not _fisheye_should_run and _stop_requested[0]:
                    self._stop_fisheye_overlay(expected=ov)
                    return
            elif s == 'fadein':
                if not _fisheye_should_run:
                    try:
                        presenter.start_fade(0.0, _FADEOUT_DUR, _on_fadeout_done)
                        _fadeout_started[0] = True
                        gpu_win.request_redraw()
                    except Exception:
                        pass
                    _state[0] = 'fadeout'
                elif not presenter.is_fading():
                    _state[0] = 'active'
            elif s == 'active':
                if not _fisheye_should_run:
                    try:
                        presenter.start_fade(0.0, _FADEOUT_DUR, _on_fadeout_done)
                        _fadeout_started[0] = True
                        gpu_win.request_redraw()
                    except Exception:
                        pass
                    _state[0] = 'fadeout'
            elif s == 'fadeout':
                if _actual_should_run and not _stop_requested[0]:
                    try:
                        presenter.start_fade(1.0, _FADEIN_DUR * 0.55)
                        gpu_win.request_redraw()
                    except Exception:
                        pass
                    _state[0] = 'fadein'
                # Otherwise the pump fires _on_fadeout_done when alpha hits 0.

            _perf_gauge('fisheye.tk_tick_ms',
                        (time.perf_counter() - _tick_t0) * 1000.0)
            try:
                self.root.after(50, _tick)
            except Exception:
                pass

        if self._destroyed:
            self._stop_fisheye_overlay()
            return

        # ── Pre-capture: grab one frame synchronously so the first
        # _tick immediately starts the fade-in instead of waiting
        # 50-150 ms for the worker thread to deliver its first frame.
        # The pre-captured frame is undistorted — at alpha ≈0.05 the
        # difference is imperceptible, and by the time alpha reaches
        # ~0.2 the worker has delivered a distorted replacement.
        try:
            import mss as _mss_pre
            with _mss_pre.mss() as _sct_pre:
                _mon_pre = (_sct_pre.monitors[1]
                            if len(_sct_pre.monitors) > 1
                            else _sct_pre.monitors[0])
                _s_pre = _sct_pre.grab(_mon_pre)
                _pre_img = Image.frombytes('RGB', _s_pre.size, _s_pre.rgb)
                _pre_rgb = _pre_img.tobytes()
                _pre_w, _pre_h = _s_pre.size.width, _s_pre.size.height
                presenter.set_frame(_pre_rgb, _pre_w, _pre_h)
                _frame_seq[0] = 1
                _latest_frame[0] = (1, _pre_rgb, _pre_w, _pre_h)
        except Exception:
            pass

        self.root.after(50, _tick)

        # ── 后台 worker: 截屏 + 畸变 + 缩放 + HUD 合成 → BGRA bytes ──
        def _worker():
            """后台线程: 全部重活在此, 主线程仅 set_frame/set_alpha."""
            import time as _time
            try:
                ctypes.windll.ole32.CoInitializeEx(0, 0)
            except Exception:
                pass
            # WGL driver serialization: this worker owns a private moderngl
            # standalone context. Without this lock its WGL ctypes calls
            # (texture upload, framebuffer use, clear, render, readback)
            # race the main-thread GLFW pump's poll_events / make_current /
            # swap_buffers / set_window_pos / set_window_size, which on
            # Windows GL drivers corrupts the main thread's saved Python
            # tstate during a click-triggered menu reflow and aborts with
            # "Fatal Python error: PyEval_RestoreThread ... GIL is released
            # (the current Python thread state is NULL)". Acquired only
            # around the actual GL calls; the heavy PIL.resize / numpy
            # storm runs without the lock so the pump never blocks long.
            try:
                from render.gpu_overlay_window import get_wgl_serialize_lock
                _wgl_lock = get_wgl_serialize_lock()
            except Exception:
                _wgl_lock = None
            _shader_gpu = bool(getattr(presenter, 'gpu_distorts', False))
            _tex_w = 0
            _tex_h = 0

            try:
                import numpy as _np
            except ImportError:
                return

            _rgb_repack_buf = [None]

            def _bgr_to_rgb_payload(frame):
                try:
                    h = int(frame.shape[0])
                    w = int(frame.shape[1])
                    buf = _rgb_repack_buf[0]
                    if buf is None or buf.shape[0] != h or buf.shape[1] != w:
                        buf = _np.empty((h, w, 3), dtype=_np.uint8)
                        _rgb_repack_buf[0] = buf
                    buf[..., 0] = frame[..., 2]
                    buf[..., 1] = frame[..., 1]
                    buf[..., 2] = frame[..., 0]
                    return (buf.tobytes(), w, h)
                except Exception:
                    return None

            # ── 鱼眼背景源: desktop(默认)/image:<path>/color:<hex> ──
            _fisheye_src = str(self._get_setting('fisheye_background_source', '') or 'desktop').strip()
            _static_frame = None
            if _fisheye_src.startswith('image:'):
                _img_path = _fisheye_src[6:].strip()
                try:
                    _src_img = Image.open(_img_path).convert('RGB').resize((sw, sh), Image.LANCZOS)
                    _static_frame = _src_img
                except Exception:
                    _static_frame = None
            elif _fisheye_src.startswith('color:'):
                _hex = _fisheye_src[6:].strip().lstrip('#')
                try:
                    r, g, b = int(_hex[0:2], 16), int(_hex[2:4], 16), int(_hex[4:6], 16)
                    _static_frame = Image.new('RGB', (sw, sh), (r, g, b))
                except Exception:
                    _static_frame = None

            _cap_fn = None
            _cap_source = ''
            if _static_frame is not None:
                _frozen = _static_frame
                def _cap_static():
                    return _frozen
                _cap_fn = _cap_static
                _cap_source = 'static'

            # ── 优先显示器快速截屏 (DXGI), fallback ImageGrab ──
            # DXGI via windows_capture pyo3 不可靠(Nuitka 下 COM/线程问题),
            # 直接用 mss (同样走 DXGI Desktop Duplication, 纯 ctypes 实现)。
            try:
                import mss as _mss_mod
                _sct = _mss_mod.mss()
                _primary = _sct.monitors[1] if len(_sct.monitors) > 1 else _sct.monitors[0]
                def _cap_mss():
                    s = _sct.grab(_primary)
                    return Image.frombytes('RGB', s.size, s.rgb)
                _cap_fn = _cap_mss
                _cap_source = 'mss'
            except Exception:
                pass
            if _cap_fn is None:
                def _cap_ig():
                    for _g in (
                        lambda: ImageGrab.grab(bbox=(0, 0, sw, sh),
                                               all_screens=True),
                        lambda: ImageGrab.grab(bbox=(0, 0, sw, sh)),
                        lambda: ImageGrab.grab(),
                    ):
                        try: return _g()
                        except Exception: continue
                    return None
                _cap_fn = _cap_ig
                _cap_source = 'imagegrab'

            _set_fisheye_capture_excluded(True)

            # ── moderngl 桶形畸变 (worker 私有 standalone context) ──
            _gl_ok = False
            _ctx = _prog = _vbo = _vao = _tex = _fbo = None
            try:
                if _shader_gpu:
                    raise RuntimeError('final-window shader path active')
                import moderngl
                import contextlib as _ctxlib
                # v2.3.15: use _wgl_lock only for context init, then
                # release it. The standalone context runs independently
                # of the GLFW pump after creation. Per-context locking
                # avoids blocking the pump for the entire GL distortion
                # pass (~2-5ms per frame).
                _init_cm = (_wgl_lock if _wgl_lock is not None
                            else _ctxlib.nullcontext())
                with _init_cm:
                    _ctx = moderngl.create_standalone_context()
                    _prog = _ctx.program(
                        vertex_shader='''
                            #version 330
                            in vec2 in_pos;
                            out vec2 uv;
                            void main() {
                                gl_Position = vec4(in_pos, 0.0, 1.0);
                                uv = in_pos * 0.5 + 0.5;
                            }
                        ''',
                        fragment_shader='''
                            #version 330
                            uniform sampler2D tex;
                            uniform float strength;
                            in vec2 uv;
                            out vec4 fragColor;
                            void main() {
                                vec2 c = uv - 0.5;
                                float r2 = dot(c, c);
                                vec2 d = uv + c * strength * r2;
                                fragColor = texture(tex, d);
                            }
                        '''
                    )
                    import numpy as _np
                    _verts = _np.array([-1, -1, 3, -1, -1, 3], dtype='f4')
                    _vbo = _ctx.buffer(_verts)
                    _vao = _ctx.simple_vertex_array(_prog, _vbo, 'in_pos')
                    # v2.3.15: input texture starts at (hw, hh) but is
                    # dynamically reallocated to match the actual screenshot
                    # size on first capture. This avoids the PIL resize
                    # bottleneck — GPU bilinear sampling handles the
                    # downscale/upscale to the FBO output size (hw×hh)
                    # automatically. The texture is rebuilt only when the
                    # screenshot resolution changes (rare — usually only
                    # on display resolution switch).
                    _tex_w, _tex_h = hw, hh  # initial; updated on first frame
                    _tex = _ctx.texture((hw, hh), 3)
                    _tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
                    _fbo = _ctx.framebuffer(
                        color_attachments=[_ctx.texture((hw, hh), 3)])
                    _prog['strength'].value = 0.55
                    _prog['tex'].value = 0
                    _gl_ok = True
            except Exception:
                _ctx = None

            # ── numpy 后备 ──
            qw, qh = (hw, hh) if _gl_ok else (int(sw * 0.5), int(sh * 0.5))
            _np_maps = None
            if not _gl_ok:
                cx_, cy_ = qw / 2.0, qh / 2.0
                _yy, _xx = _np.mgrid[0:qh, 0:qw].astype(_np.float32)
                _nx = (_xx - cx_) / cx_;  _ny = (_yy - cy_) / cy_
                _r2 = _nx * _nx + _ny * _ny; _f = 1.0 + 0.55 * _r2
                _sx = _np.clip(cx_ + _nx * _f * cx_, 0.0, qw - 1.0001)
                _sy = _np.clip(cy_ + _ny * _f * cy_, 0.0, qh - 1.0001)
                _x0 = _sx.astype(_np.int32); _x1 = _x0 + 1
                _y0 = _sy.astype(_np.int32); _y1 = _y0 + 1
                _wfx = (_sx - _x0).astype(_np.float32)[..., _np.newaxis]
                _wfy = (_sy - _y0).astype(_np.float32)[..., _np.newaxis]
                _np_maps = (_x0, _x1, _y0, _y1, _wfx, _wfy)

            _frame_interval = 1.0 / 60.0

            # ── 预生成 HUD 叠加 (RGBA premultiplied, 一次性) ──
            # v2.3.10: 改在 (out_w, out_h) 分辨率合成 (而不是 sw×sh)，
            # GPU presenter 把这个纹理 bilinear 拉伸到全屏窗口，零 CPU
            # 开销。HUD 像素几何按相同比例缩放；视觉一致。
            out_w, out_h = qw, qh
            _hud_bgra_premult = None  # numpy uint8 (out_h, out_w, 4) BGRA premult
            _hud_inv_a_u16 = None     # numpy uint16 (out_h, out_w, 1) 255 - alpha
            try:
                from PIL import ImageDraw as _IDraw
                _hud_w, _hud_h = out_w, out_h
                _hud = Image.new('RGBA', (_hud_w, _hud_h), (0, 0, 0, 0))
                _hd = _IDraw.Draw(_hud)
                _hd.rectangle((0, 0, _hud_w, _hud_h), fill=(0, 0, 0, 115))
                for _sy2 in range(0, _hud_h, 3):
                    _hd.line([(0, _sy2), (_hud_w, _sy2)], fill=(0, 0, 0, 18))
                _line_positions = [
                    int(_hud_h * 0.08), int(_hud_h * 0.15),
                    int(_hud_h * 0.85), int(_hud_h * 0.92),
                ]
                for _ly in _line_positions:
                    _hd.line([(int(_hud_w * 0.05), _ly),
                              (int(_hud_w * 0.95), _ly)],
                             fill=(156, 236, 255, 35), width=1)
                _cx2, _cy2 = _hud_w // 2, _hud_h // 2
                _hd.line([(_cx2 - 40, _cy2), (_cx2 - 12, _cy2)], fill=(156, 236, 255, 45), width=1)
                _hd.line([(_cx2 + 12, _cy2), (_cx2 + 40, _cy2)], fill=(156, 236, 255, 45), width=1)
                _hd.line([(_cx2, _cy2 - 40), (_cx2, _cy2 - 12)], fill=(156, 236, 255, 45), width=1)
                _hd.line([(_cx2, _cy2 + 12), (_cx2, _cy2 + 40)], fill=(156, 236, 255, 45), width=1)
                _blen = 50
                _bpad = int(_hud_w * 0.04)
                _bpad_y = int(_hud_h * 0.05)
                _bc = (156, 236, 255, 55)
                _hd.line([(_bpad, _bpad_y), (_bpad + _blen, _bpad_y)], fill=_bc, width=1)
                _hd.line([(_bpad, _bpad_y), (_bpad, _bpad_y + _blen)], fill=_bc, width=1)
                _hd.line([(_hud_w - _bpad, _bpad_y), (_hud_w - _bpad - _blen, _bpad_y)], fill=_bc, width=1)
                _hd.line([(_hud_w - _bpad, _bpad_y), (_hud_w - _bpad, _bpad_y + _blen)], fill=_bc, width=1)
                _hd.line([(_bpad, _hud_h - _bpad_y), (_bpad + _blen, _hud_h - _bpad_y)], fill=_bc, width=1)
                _hd.line([(_bpad, _hud_h - _bpad_y), (_bpad, _hud_h - _bpad_y - _blen)], fill=_bc, width=1)
                _hd.line([(_hud_w - _bpad, _hud_h - _bpad_y), (_hud_w - _bpad - _blen, _hud_h - _bpad_y)], fill=_bc, width=1)
                _hd.line([(_hud_w - _bpad, _hud_h - _bpad_y), (_hud_w - _bpad, _hud_h - _bpad_y - _blen)], fill=_bc, width=1)
                hud_arr = _np.array(_hud, dtype=_np.uint8)  # RGBA, top-down
                # 转 BGRA premult (uint8) — 一次性，循环里直接复用
                a_u8 = hud_arr[..., 3:4]
                a_f = a_u8.astype(_np.float32) / 255.0
                rgb_pre = (hud_arr[..., :3].astype(_np.float32) * a_f
                           ).astype(_np.uint8)
                bgra = _np.empty_like(hud_arr)
                bgra[..., 0] = rgb_pre[..., 2]   # B
                bgra[..., 1] = rgb_pre[..., 1]   # G
                bgra[..., 2] = rgb_pre[..., 0]   # R
                bgra[..., 3] = a_u8[..., 0]
                _hud_bgra_premult = bgra
                # 预算 (255 - alpha) 的 uint16 形式，循环内整数乘法用
                _hud_inv_a_u16 = (255 - a_u8.astype(_np.uint16))  # (h,w,1)
            except Exception:
                _hud_bgra_premult = None
                _hud_inv_a_u16 = None

            # 复用缓冲：循环内 _np.array(...) 与 alpha 临时数组的分配
            # 是主要 GIL 占用点；预分配到 (out_h, out_w, 4) 后用切片赋值替代。
            # bgra_buf 既是工作区也是最终输出。
            _bgra_buf = _np.empty((out_h, out_w, 4), dtype=_np.uint8)
            _bgra_buf[..., 3] = 255  # 全不透明
            _hud_tmp = _np.empty((out_h, out_w, 4), dtype=_np.uint16)

            _timer_res = None
            try:
                from render.overlay_scheduler import _WinTimerResolution
                _timer_res = _WinTimerResolution()
                _timer_res.acquire()
            except Exception:
                pass

            _fisheye_diag_logged = [False]
            while _running[0]:
                _t_start = _time.time()
                shot = _cap_fn()
                if shot is None or not _running[0]:
                    _time.sleep(0.05)
                    continue
                if not _fisheye_diag_logged[0]:
                    _fisheye_diag_logged[0] = True
                    _diag_size = (shot[1], shot[2]) if isinstance(shot, tuple) else shot.size
                    print(f'[SAO-UI] fisheye worker: shot={_diag_size}, '
                          f'final_shader={int(_shader_gpu)}, '
                          f'gl_ok={_gl_ok}, tex=({_tex_w},{_tex_h}), '
                          f'fbo=({hw},{hh})')
                try:
                    if _shader_gpu:
                        if isinstance(shot, tuple):
                            _rgb_bytes, _shot_w, _shot_h = shot
                        else:
                            if getattr(shot, 'mode', 'RGB') != 'RGB':
                                shot = shot.convert('RGB')
                            _rgb_bytes = shot.tobytes()
                            _shot_w, _shot_h = shot.size
                        _frame_seq[0] += 1
                        _latest_frame[0] = (
                            _frame_seq[0], _rgb_bytes, _shot_w, _shot_h)
                        try:
                            presenter.set_frame(_rgb_bytes, _shot_w, _shot_h)
                            gpu_win.request_redraw()
                        except Exception:
                            pass
                        _elapsed = _time.time() - _t_start
                        _perf_gauge('fisheye.worker.frame_ms', _elapsed * 1000.0)
                        _sleep = max(0.001, _frame_interval - _elapsed)
                        _time.sleep(_sleep)
                        continue
                    if isinstance(shot, tuple):
                        _rgb_bytes, _shot_w, _shot_h = shot
                        shot = Image.frombytes('RGB', (_shot_w, _shot_h), _rgb_bytes)
                    if _gl_ok:
                        # v2.3.15: feed screenshot directly to GPU texture.
                        # The texture is rebuilt only when shot size changes
                        # (rare — same display stays constant). GPU bilinear
                        # sampling handles any scale to the FBO (hw×hh).
                        # Eliminates PIL resize (~3-8ms saved per frame).
                        _shot_w, _shot_h = shot.size
                        _need_rebuild = (_shot_w != _tex_w or _shot_h != _tex_h)
                        _shot_bytes = shot.tobytes()
                        # All GL calls (including texture rebuild) must
                        # happen inside _wgl_lock to avoid racing the
                        # GLFW pump's WGL context on the same driver.
                        # v2.3.17: retry up to 3× (2 ms each) so the worker
                        # doesn't starve when the pump holds the lock briefly.
                        if _wgl_lock is not None:
                            _wait_t0 = _time.perf_counter()
                            _acquired = False
                            for _retry in range(3):
                                if _wgl_lock.acquire(blocking=False):
                                    _acquired = True
                                    break
                                _time.sleep(0.002)
                            if not _acquired:
                                _phase_trace('fisheye.gl.skip', 'wgl_lock_busy')
                                _time.sleep(0.006)
                                continue
                            try:
                                _wait_ms = (_time.perf_counter() - _wait_t0) * 1000.0
                                _perf_gauge('fisheye.gl.wait_ms', _wait_ms)
                                _phase_trace('fisheye.gl.acquired')
                                if _need_rebuild:
                                    try:
                                        _tex.release()
                                    except Exception:
                                        pass
                                    _tex = _ctx.texture((_shot_w, _shot_h), 3)
                                    _tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
                                    _tex_w, _tex_h = _shot_w, _shot_h
                                _tex.write(_shot_bytes)
                                _fbo.use()
                                _ctx.clear()
                                _tex.use(0)
                                _vao.render(moderngl.TRIANGLES)
                                raw = _fbo.color_attachments[0].read()
                                _phase_trace('fisheye.gl.end')
                            finally:
                                try:
                                    _wgl_lock.release()
                                except Exception:
                                    pass
                        else:
                            if _need_rebuild:
                                try:
                                    _tex.release()
                                except Exception:
                                    pass
                                _tex = _ctx.texture((_shot_w, _shot_h), 3)
                                _tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
                                _tex_w, _tex_h = _shot_w, _shot_h
                            _tex.write(_shot_bytes)
                            _fbo.use()
                            _ctx.clear()
                            _tex.use(0)
                            _vao.render(moderngl.TRIANGLES)
                            raw = _fbo.color_attachments[0].read()
                        dist = Image.frombytes('RGB', (hw, hh), raw)
                    else:
                        tiny = shot.resize((qw, qh), Image.BILINEAR)
                        _x0, _x1, _y0, _y1, _wfx, _wfy = _np_maps
                        a = _np.array(tiny, dtype=_np.float32)
                        t = a[_y0, _x0] * (1 - _wfx) + a[_y0, _x1] * _wfx
                        b = a[_y1, _x0] * (1 - _wfx) + a[_y1, _x1] * _wfx
                        dist = Image.fromarray(
                            (t * (1 - _wfy) + b * _wfy)
                            .clip(0, 255).astype(_np.uint8))
                except Exception as _fisheye_err:
                    # v2.3.15: log first error to help diagnose blank fisheye
                    if not getattr(self, '_fisheye_err_logged', False):
                        self._fisheye_err_logged = True
                        print(f'[SAO-UI] fisheye worker frame error: {_fisheye_err}')
                        import traceback as _tb
                        _tb.print_exc()
                    _time.sleep(0.02)
                    continue
                if not _running[0]:
                    break
                # v2.3.10: 不再 PIL 放大到 (sw,sh)。直接在 (hw,hh)
                # 出帧；GPU presenter 把纹理 bilinear 拉伸到全屏窗口。
                # 这一步去掉了一个全屏 PIL.resize + 一个 sw×sh 的
                # numpy 数组 + 三个全屏 float32 临时数组（每帧 ~33MB
                # × 3）。1080p 下每帧分配从 ~130MB 降到 ~6MB。
                rgb_arr = _np.asarray(dist, dtype=_np.uint8)  # (out_h, out_w, 3)
                # 写入 BGRA 工作缓冲：用 slice 反转通道，避免逐通道写
                _bgra_buf[..., :3] = rgb_arr[..., ::-1]
                # alpha 通道初始化时已设为 255；HUD 合成不会改它
                if (_hud_bgra_premult is not None
                        and _hud_inv_a_u16 is not None):
                    _np.copyto(_hud_tmp, _bgra_buf, casting='unsafe')
                    _hud_tmp *= _hud_inv_a_u16
                    _hud_tmp //= 255
                    _hud_tmp += _hud_bgra_premult
                    _np.clip(_hud_tmp, 0, 255, out=_hud_tmp)
                    _np.copyto(_bgra_buf, _hud_tmp, casting='unsafe')
                _frame_seq[0] += 1
                _bgra_bytes = _bgra_buf.tobytes()
                _latest_frame[0] = (_frame_seq[0], _bgra_bytes, out_w, out_h)
                try:
                    presenter.set_frame(_bgra_bytes, out_w, out_h)
                    gpu_win.request_redraw()
                except Exception:
                    pass
                _elapsed = _time.time() - _t_start
                _perf_gauge('fisheye.worker.frame_ms', _elapsed * 1000.0)
                _sleep = max(0.001, _frame_interval - _elapsed)
                _time.sleep(_sleep)

            if _ctx:
                try: _ctx.release()
                except Exception: pass
            if _timer_res:
                try: _timer_res.release()
                except Exception: pass

        import threading as _th
        _worker_thread = _th.Thread(target=_worker, daemon=True)
        ov._worker_thread = _worker_thread
        _worker_thread.start()

    @_probe.decorate('ui.fisheye.stop')
    def _stop_fisheye_overlay(self, wait: bool = False, expected=None):
        """销毁持久鱼眼叠加层.

        v3.1.8 round 19: previously this method called ``worker_thread.join(2.0)``
        and ``gpu_win.destroy()`` inline on the Tk main thread, which could
        stall the UI for up to ~2 seconds while waiting for the fisheye
        worker to acknowledge stop. Now: the main thread does only the
        light Tk-bound work (hit-layer destroy, zorder release) and flips
        the ``running[0]`` stop bit synchronously, then dispatches the
        heavy wait + GPU teardown to a daemon thread so the user sees an
        instant UI response. During final app close, pass wait=True so the
        GLFW pump/GPU resources are torn down before root.quit().
        """
        ov = self._fisheye_ov
        if expected is not None and ov is not expected:
            return
        if ov is None:
            return
        gpu_win = getattr(ov, 'gpu_win', None)
        pump = getattr(gpu_win, '_pump', None) if gpu_win is not None else None
        pump_thread = getattr(pump, '_thread', None)
        if pump_thread is not None and threading.current_thread() is pump_thread:
            post_to_tk = getattr(pump, 'post_to_tk', None)
            if callable(post_to_tk):
                try:
                    post_to_tk(lambda: self._stop_fisheye_overlay(
                        wait=wait, expected=expected))
                    return
                except Exception:
                    pass

        self._destroy_fisheye_hit_layer()
        self._fisheye_ov = None
        if gpu_win is not None:
            # Light Win32 ex-style flip; keep on main for ordering safety.
            self._release_fisheye_input_zorder(ov)
        # Tell the worker to stop ASAP — it polls running[0] on every frame.
        running = getattr(ov, '_running_ref', None)
        if running:
            running[0] = False
        # Heavy cleanup (worker join + GPU destroy + presenter release) on a
        # daemon thread so the main loop returns immediately. Legacy Tk
        # Toplevel destroy must still go via root.after so it lands on main.
        _root = self.root

        def _async_fisheye_shutdown(_ov=ov, _root_ref=_root):
            worker_thread = getattr(_ov, '_worker_thread', None)
            if worker_thread is not None and worker_thread.is_alive():
                try:
                    worker_thread.join(timeout=2.0)
                    if worker_thread.is_alive():
                        _phase_trace('fisheye.stop.join_timeout',
                                     getattr(worker_thread, 'name', 'worker'))
                except Exception:
                    pass
            gw = getattr(_ov, 'gpu_win', None)
            presenter = getattr(_ov, 'presenter', None)
            if gw is not None:
                try:
                    gw.destroy()
                except Exception:
                    pass
                try:
                    _ov.gpu_win = None
                except Exception:
                    pass
                presenter = None
            if presenter is not None:
                # Only fall back to direct release when there is no GPU
                # window/context left to marshal through.
                try:
                    presenter.release()
                except Exception:
                    pass
                try:
                    _ov.presenter = None
                except Exception:
                    pass
            destroy_cb = getattr(_ov, 'destroy', None)
            if callable(destroy_cb):
                # Legacy Tk Toplevel — must land on main thread.
                if wait:
                    try:
                        destroy_cb()
                    except Exception:
                        pass
                else:
                    post_to_tk = getattr(pump, 'post_to_tk', None)
                    if callable(post_to_tk):
                        try:
                            post_to_tk(destroy_cb)
                        except Exception:
                            pass
                    else:
                        try:
                            _root_ref.after(0, destroy_cb)
                        except Exception:
                            pass

        if wait:
            _async_fisheye_shutdown()
            return
        try:
            threading.Thread(
                target=_async_fisheye_shutdown,
                name='sao-fisheye-stop',
                daemon=True,
            ).start()
        except Exception:
            # If thread creation fails, fall back to inline (better than leak).
            _async_fisheye_shutdown()

    # ══════════════════════════════════════════════
    #  LinkStart 入场鱼眼镜头畅变
    # ══════════════════════════════════════════════
    def _run_fisheye_entry(self):
        """
        LinkStart 结束后短暂鱼眼镜头畟变过渡 — 屏幕从弯曲收缩至正常.

        流程: 抓取当前屏幕 → 应用桶形型畟变 (MESH变换) →
                全屏覆盖层显示畟变图 → 0.9s内渐隐 →
                真实 UI 从底层透出 (SAO 镜头对焦效果).
        """
        try:
            from PIL import ImageGrab, Image, ImageTk
        except ImportError:
            return

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        img = None
        for _grab in (
            lambda: ImageGrab.grab(bbox=(0, 0, sw, sh), all_screens=True),
            lambda: ImageGrab.grab(bbox=(0, 0, sw, sh)),
            lambda: ImageGrab.grab(),
        ):
            try:
                img = _grab()
                break
            except Exception:
                continue
        if img is None:
            return

        # 半分辨率处理 (MESH 运算量 1/4)
        half_w, half_h = sw // 2, sh // 2
        small = img.resize((half_w, half_h), Image.BILINEAR)

        def _barrel(src, strength):
            cx_, cy_ = half_w / 2.0, half_h / 2.0
            grid = 18
            mesh_data = []
            for gy in range(0, half_h, grid):
                for gx in range(0, half_w, grid):
                    x1, y1 = gx, gy
                    x2, y2 = min(gx + grid, half_w), min(gy + grid, half_h)
                    src_pts = []
                    for px, py in [(x1, y1), (x1, y2), (x2, y2), (x2, y1)]:
                        nx_ = (px - cx_) / cx_
                        ny_ = (py - cy_) / cy_
                        r2 = nx_ * nx_ + ny_ * ny_
                        f = 1.0 + strength * r2
                        sx = cx_ + nx_ * f * cx_
                        sy = cy_ + ny_ * f * cy_
                        src_pts.extend([
                            max(0.0, min(half_w - 1.0, sx)),
                            max(0.0, min(half_h - 1.0, sy)),
                        ])
                    mesh_data.append(((x1, y1, x2, y2), src_pts))
            return src.transform(src.size, Image.MESH, mesh_data, Image.BILINEAR)

        try:
            dist_half = _barrel(small, 0.50)
            distorted = dist_half.resize((sw, sh), Image.BILINEAR)
        except Exception:
            return

        # 全屏 overlay
        ov = tk.Toplevel(self.root)
        ov.overrideredirect(True)
        ov.attributes('-topmost', True)
        ov.attributes('-alpha', 1.0)
        ov.geometry(f'{sw}x{sh}+0+0')
        cv_ov = tk.Canvas(ov, width=sw, height=sh,
                          highlightthickness=0, bg='black')
        cv_ov.pack(fill=tk.BOTH, expand=True)
        photo = ImageTk.PhotoImage(distorted)
        cv_ov.create_image(0, 0, image=photo, anchor='nw')
        cv_ov._photo = photo  # 防止 GC

        # ease-in 渐隐: 开始快, 收尾慢 (0.9s 内全透明)
        t0 = time.time()
        dur = 0.90

        def _fade():
            if self._destroyed:
                try: ov.destroy()
                except: pass
                return
            elapsed = time.time() - t0
            if elapsed >= dur:
                try:
                    ov.destroy()
                except Exception:
                    pass
                return
            a = max(0.0, 1.0 - (elapsed / dur) ** 0.6)
            try:
                ov.attributes('-alpha', a)
            except Exception:
                pass
            try:
                self.root.after(16, _fade)
            except Exception:
                pass

        _fade()
