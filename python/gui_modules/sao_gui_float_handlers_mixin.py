# -*- coding: utf-8 -*-
"""
SAOPlayerGUIFloatHandlersMixin — fifteenth mixin extracted from
SAOPlayerGUI (round 58 of the sao_gui split refactor). 9 methods,
~138 lines.

A grab-bag for the remaining small handlers on / around the
floating SAO trigger button: drag / hover / context-menu /
z-order maintenance + a handful of unrelated small helpers that
didn't have a natural home in the earlier mixins.

Methods:
  * _float_enter(e) — mouse enter (hover state on).
  * _float_leave(e) — mouse leave (hover state off).
  * _lift_float_loop — periodic root.after lift() loop to keep
    the float button above the fisheye overlay (Win32 z-order
    maintenance fallback).
  * _raise_panel_window(panel) — bring a floating panel to the
    front + take focus.
  * _setup_hotkeys — wire the SAOHotkeyManager to its 7 handlers.

Required SAOPlayerGUI attrs:
  * self._float, self._float_hwnd, self._float_alpha,
    self._float_drag_origin, self._float_drag_active,
    self._ctx_menu_open, self._lift_loop_active
  * self._fisheye_ov, self._sao_menu, self._panels_hidden,
    self.root, self.settings
  * self._hotkey_mgr

Required SAOPlayerGUI methods (via MRO):
  * _toggle_sao_menu (Menu mixin)
  * _toggle_recognition_menu, _toggle_topmost,
    _toggle_hide_all_panels (Panels mixin)
  * Game-specific toggles provided by plugin
  * _show_plugin_popup_menu (Panels mixin)
"""

from __future__ import annotations

import ctypes
import tkinter as tk
import time

from utils.sao_sound import get_cjk_font
from gui_modules.sao_hotkey_manager import SAOHotkeyManager
from gui_modules.sao_panel_ui import (
    _apply_window_icon, _disable_native_window_shadow,
)


# Win32 user32 (used by _create_floating_widget's WS_EX flags). Mirrors
# the same _user32 handle that sao_panel_ui uses; ctypes.windll caches
# module handles so both modules share the same object.
_user32 = ctypes.windll.user32


class SAOPlayerGUIFloatHandlersMixin:
    """Mixin bundling float-button drag handlers + small misc helpers."""

    # Round 76 of sao_gui split refactor: _float_click / _float_drag /
    # _float_release were dead code. _create_floating_widget binds the
    # float button's <Button-1> directly to _toggle_sao_menu (round 56);
    # these three handlers were never wired to a Tk event and they
    # referenced self._drag which was never initialized — confirmed via
    # pre-refactor git blame (always dead, predates the split).
    # Removed to fix the latent AttributeError + clear the noise.

    def _float_enter(self, e):
        """高亮悬浮菜单按钮"""
        try:
            self._float_hover = True
            self._float_alpha = 1.0
            self._refresh_float_layered()
        except Exception:
            pass

    def _float_leave(self, e):
        """恢复默认色"""
        try:
            self._float_hover = False
            self._float_alpha = 1.0   # 完全不透明 — 覆盖游戏原生条
            self._refresh_float_layered()
        except Exception:
            pass

    def _lift_float_loop(self):
        """SAO 菜单开启时持续将悬浮按钮保持在最上层.

        v3.1.9 round 22: cadence bumped from 150 ms (6.7 Hz) to 250 ms
        (4 Hz). The float button is the small floating menu/status badge
        and the user can't perceive the 100 ms-longer cover-recovery
        delay, but cutting the per-second SetWindowPos calls from ~7
        to ~4 trims ~100-300 us/sec of main-thread work whenever the
        SAO menu is open.
        """
        if self._destroyed or not self._lift_loop_active:
            return
        try:
            if self._float.winfo_exists():
                self._float.lift()
                self._sync_float_button_geometry(show=True)
        except Exception:
            pass
        try:
            self.root.after(250, self._lift_float_loop)
        except Exception:
            pass

    def _raise_panel_window(self, panel):
        """把面板提到最前并取焦, 防止被 SAO overlay 或其他 topmost 挡住."""
        if panel is None:
            return
        win = getattr(panel, '_win', None)
        if win is None:
            return
        def _apply(force_focus: bool = False):
            try:
                if not win.winfo_exists():
                    return
                try:
                    if str(win.state()) == 'withdrawn':
                        return
                except Exception:
                    pass
                win.attributes('-topmost', True)
                win.lift()
                if force_focus:
                    win.focus_force()
            except Exception:
                pass
        _apply(force_focus=True)
        # Many plugin panels briefly demote themselves at ~220ms after
        # show(). Re-assert topmost just after that so Entity panels do not
        # disappear behind the game when the user clicks elsewhere.
        for delay in (260, 520):
            try:
                self.root.after(delay, _apply)
            except Exception:
                pass

    def _setup_hotkeys(self):
        self._hotkey_mgr = SAOHotkeyManager(self.settings, {
            'toggle_recognition': lambda: self.root.after(0, self._toggle_recognition_menu),
            'toggle_topmost': lambda: self.root.after(0, self._toggle_topmost),
            'hide_panels': lambda: self.root.after(0, self._toggle_hide_all_panels),
            'show_plugins': lambda: self.root.after(0, self._show_plugin_popup_menu),
            'toggle_float_button': lambda: self.root.after(0, self._toggle_float_button_visibility),
            'toggle_sao_menu': lambda: self.root.after(0, lambda: self._toggle_sao_menu(allow_close=True)),
        }, hotkey_provider=self._plugin_hotkey_map)

    def _plugin_hotkey_map(self):
        """Resolve plugin-registered hotkeys for the hotkey listener.

        Returns ``{action: {'key': 'F6', 'callback': fn}}`` for active plugins,
        honouring a user override in ``settings['hotkeys']`` over the plugin's
        declared default. Plugin callbacks are marshalled to the Tk thread.
        """
        out = {}
        try:
            from act_platform.runtime import ensure_act_plugin_manager
            mgr = ensure_act_plugin_manager(self, load=False)
            saved = self.settings.get('hotkeys', {}) or {}
            for hk in mgr.list_hotkeys():
                if not hk.get('active'):
                    continue
                action = str(hk.get('action') or '')
                key = ''
                if isinstance(saved, dict) and action in saved:
                    v = saved[action]
                    key = (v.get('key') or v.get('name')) if isinstance(v, dict) else str(v)
                key = str(key or hk.get('default_key') or '').upper()
                if not key:
                    continue
                out[action] = {
                    'key': key,
                    'callback': (lambda a=action, m=mgr: self.root.after(0, lambda: m.dispatch_hotkey(a))),
                }
        except Exception:
            pass
        return out

    def _create_floating_widget(self):
        """NerveGear 按钮 — SAO 菜单可见入口 (GPU 绘制).

        左键打开 SAO 菜单, 右键迷你上下文菜单 (主题切换/关于/退出).
        位置: 屏幕右下角, 可拖动, 位置持久化到 settings.
        空闲时有呼吸辉光动画.
        """
        from gui_modules.sao_gui_nervegear_button import SIZE as NG_SIZE, GpuNerveGearButton

        try:
            _sw = self.root.winfo_screenwidth()
            _sh = self.root.winfo_screenheight()
        except Exception:
            _sw, _sh = 1920, 1080

        FW = NG_SIZE
        FH = NG_SIZE
        self._fw, self._fh = FW, FH
        self._float_alpha = 0.95
        self._float_hover = False
        self._ng_drag_start = None

        saved_pos = self._get_setting('nervegear_button_pos', None)
        if saved_pos and isinstance(saved_pos, (list, tuple)) and len(saved_pos) == 2:
            ng_x, ng_y = int(saved_pos[0]), int(saved_pos[1])
        else:
            ng_x = _sw - NG_SIZE - 20
            ng_y = _sh - NG_SIZE - 60

        # Tk owner for dialogs, menu geometry and LinkStart animation. The
        # visible trigger below is a GLFW/ModernGL overlay.
        self._float = tk.Toplevel(self.root)
        self._float.overrideredirect(True)
        self._float.attributes('-topmost', True)
        self._float.attributes('-alpha', 0.0)
        self._float.geometry(f'{FW}x{FH}+{ng_x}+{ng_y}')
        self._float.configure(bg='#010101')
        _apply_window_icon(self._float)

        self._float_hwnd = 0
        try:
            self._float.update_idletasks()
            GWL_EXSTYLE = -20
            WS_EX_TOOLWINDOW = 0x00000080
            WS_EX_LAYERED = 0x00080000
            WS_EX_TRANSPARENT = 0x00000020
            WS_EX_NOACTIVATE = 0x08000000
            hwnd = int(_user32.GetParent(ctypes.c_void_p(self._float.winfo_id())))
            self._float_hwnd = hwnd
            style = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
            style = (style | WS_EX_LAYERED | WS_EX_TRANSPARENT
                     | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
            _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE, style)
            _disable_native_window_shadow(self._float)
            try:
                self.register_wnd_shield(hwnd)
            except Exception:
                pass
        except Exception:
            self._float_hwnd = 0

        self._float_display_name = 'SAO'

        self._ng_theme = self._get_setting('nervegear_theme', 'dark') or 'dark'
        self._ng_glow_phase = 0.0
        self._ng_breath_after = None

        # ── 右键菜单 (精简: 平台操作) ──
        self._float_ctx = tk.Menu(self.root, tearoff=0,
                                  bg='#0f121a', fg='#d0e8f0',
                                  activebackground='#f3af12',
                                  activeforeground='#ffffff',
                                  relief='flat', bd=1,
                                  font=get_cjk_font(9))
        self._float_ctx.add_command(label='◆ 打开 SAO 菜单', command=self._toggle_sao_menu)
        self._float_ctx.add_separator()
        self._float_ctx.add_command(label='🌙 切换主题 Dark/Light',
                                    command=self._toggle_nervegear_theme)
        self._float_ctx.add_command(label='◈ 隐藏/显示面板', command=self._toggle_hide_all_panels)
        self._float_ctx.add_command(label='◇ WebView UI', command=self._switch_to_webview_ui)
        self._float_ctx.add_command(label='◇ 关于', command=self._show_about)
        self._float_ctx.add_command(label='✕ 退出', command=self._on_close)
        def _show_ctx_menu_at(x_root: int, y_root: int):
            self._ctx_menu_open = True
            try:
                menu_h = 140
                popup_x = int(x_root)
                popup_y = max(0, int(y_root) - menu_h)
                self._float_ctx.tk_popup(popup_x, popup_y)
            except Exception:
                self._float_ctx.tk_popup(int(x_root), int(y_root))
            finally:
                self._ctx_menu_open = False

        def _show_ctx_menu(e):
            _show_ctx_menu_at(e.x_root, e.y_root)

        self._float.bind('<Button-3>', _show_ctx_menu)

        def _on_gpu_hover(active: bool):
            self._float_hover = bool(active)
            self._float_alpha = 1.0

        def _on_gpu_move_end(x: int, y: int):
            try:
                self._float.geometry(f'{FW}x{FH}+{int(x)}+{int(y)}')
                self._set_setting('nervegear_button_pos', [int(x), int(y)])
            except Exception:
                pass

        def _on_gpu_click():
            def _open_menu():
                try:
                    self._toggle_sao_menu()
                except Exception as exc:
                    print(
                        f'[SAO] GPU NerveGear click failed to open SAO menu: '
                        f'{type(exc).__name__}: {exc}',
                        flush=True,
                    )
            try:
                self.root.after_idle(_open_menu)
            except Exception:
                _open_menu()

        try:
            self._float_gpu_button = GpuNerveGearButton(
                self.root,
                ng_x,
                ng_y,
                theme=self._ng_theme,
                on_click=_on_gpu_click,
                on_right_click=_show_ctx_menu_at,
                on_move_end=_on_gpu_move_end,
                on_hover=_on_gpu_hover,
            )
        except Exception as exc:
            self._float_gpu_button = None
            print(
                f'[SAO] GPU NerveGear button unavailable: '
                f'{type(exc).__name__}: {exc}',
                flush=True,
            )
            raise

        def _render_ng():
            btn = getattr(self, '_float_gpu_button', None)
            if btn is None:
                return
            try:
                btn.set_theme(self._ng_theme)
                btn.set_hover(bool(getattr(self, '_float_hover', False)))
                btn.set_alpha(float(getattr(self, '_float_alpha', 1.0) or 0.0))
                btn.render(self._ng_glow_phase)
            except Exception:
                pass

        self._render_ng = _render_ng
        _render_ng()

        self._float.withdraw()
        self._float_button_hidden = False
        self._sync_float_button_geometry(show=False)
        try:
            self.root.after(2500, self._ensure_float_button_visible)
        except Exception:
            pass
        try:
            self.root.after(3000, self._start_topmost_loop)
        except Exception:
            pass

    def _ensure_float_button_visible(self):
        """Fail-safe: show the GPU trigger if startup animation stalls."""
        if getattr(self, '_destroyed', False):
            return
        btn = getattr(self, '_float_gpu_button', None)
        if btn is None or getattr(btn, '_visible', False):
            return
        try:
            if self._float and self._float.winfo_exists():
                self._float.deiconify()
                self._float.lift()
        except Exception:
            pass
        self._set_float_alpha(0.95)
        self._sync_float_button_geometry(show=True)

    def _start_topmost_loop(self):
        """Re-assert TOPMOST on the GPU trigger button every 2s."""
        if getattr(self, '_destroyed', False):
            return
        btn = getattr(self, '_float_gpu_button', None)
        if btn is not None and getattr(btn, '_visible', False):
            try:
                btn.raise_topmost()
            except Exception:
                pass
        try:
            self.root.after(2000, self._start_topmost_loop)
        except Exception:
            pass

    def _toggle_float_button_visibility(self):
        """Insert key: hide/show ALL overlay layers (compositor + button)."""
        try:
            from render.gpu_overlay_window import (
                get_unified_overlay_mode, _unified_overlay_instance)
            if get_unified_overlay_mode() and _unified_overlay_instance:
                uo = _unified_overlay_instance
                host = uo.host
                if host:
                    if getattr(self, '_overlays_hidden', False):
                        host.show()
                        self._overlays_hidden = False
                    else:
                        host.hide()
                        self._overlays_hidden = True
                    return
        except Exception:
            pass
        btn = getattr(self, '_float_gpu_button', None)
        if btn is None:
            return
        if getattr(btn, '_visible', False):
            btn.withdraw()
            self._float_button_hidden = True
        else:
            btn.deiconify()
            btn.raise_topmost()
            self._float_button_hidden = False

    def _toggle_nervegear_theme(self):
        """Toggle NerveGear button between dark and light theme."""
        self._ng_theme = 'light' if getattr(self, '_ng_theme', 'dark') == 'dark' else 'dark'
        try:
            self._set_setting('nervegear_theme', self._ng_theme)
        except Exception:
            pass
        render_fn = getattr(self, '_render_ng', None)
        if callable(render_fn):
            render_fn()

    def _start_nervegear_glow(self):
        """Breathing glow animation for the NerveGear button."""
        if getattr(self, '_ng_breath_after', None) is not None:
            return
        import math
        def _step():
            if self._destroyed:
                return
            self._ng_glow_phase = (time.time() * 0.8) % (2 * math.pi)
            render_fn = getattr(self, '_render_ng', None)
            if callable(render_fn):
                try:
                    render_fn()
                except Exception:
                    pass
            self._ng_breath_after = self.root.after(50, _step)
        self._ng_breath_after = self.root.after(50, _step)

    def _stop_nervegear_glow(self):
        after_id = getattr(self, '_ng_breath_after', None)
        if after_id is not None:
            try:
                self.root.after_cancel(after_id)
            except Exception:
                pass
            self._ng_breath_after = None

    # ══════════════════════════════════════════════
    #  识别引擎
    # ══════════════════════════════════════════════
