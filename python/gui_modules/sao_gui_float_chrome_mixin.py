# -*- coding: utf-8 -*-
"""
SAOPlayerGUIFloatChromeMixin — fourteenth mixin extracted from
SAOPlayerGUI (round 56 of the sao_gui split refactor). 20 methods,
~409 lines.

The two clusters bundled together because they're both peripheral chrome around
the platform float button and menu animations. Game-specific overlay handlers
are installed by plugins.

Float button + breath + animation:
  * _set_float_alpha — Win32 SetLayeredWindowAttributes wrapper
  * _start_float_breath / _breath_step / _stop_float_breath —
    idle-state ~2 Hz vertical sine wobble
  * _attach_panel_float / _panel_float_shared_tick — wire a
    panel into the shared 30 fps panel-float scheduler
    * _update_float_display / _update_float_status — trigger refresh helpers
  * _animate_float_to — drag-end snap animation
  * _play_motion_blur (146 lines — biggest in this cluster) —
    radial blur effect on SAO menu open/close (background-threaded
    screen grab + radial blur + main-thread fade)

Required SAOPlayerGUI attrs:
    * self._float, self._fw, self._fh, self._float_name_lbl
  * self._breath_active, self._breath_after_id, self._breath_base_x,
    self._breath_base_y, self._breath_phase, self._breath_amp
  * self._panel_float_after_id, self._panel_float_attached
    * self._sao_menu, self.root, self.settings

Required SAOPlayerGUI methods (via MRO):
  * _toggle_sao_menu (Menu mixin)
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Optional

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]

from utils.perf_probe import probe as _probe, gauge as _perf_gauge
from sao_theme import ease_out


class SAOPlayerGUIFloatChromeMixin:
    """Mixin bundling float button, breath, motion blur and panel-float animations."""

    def _refresh_float_layered(self):
        """Refresh the GPU-presented NerveGear trigger, if attached."""
        render_fn = getattr(self, '_render_ng', None)
        if callable(render_fn):
            try:
                render_fn()
            except Exception:
                pass

    def _sync_float_button_geometry(self, show: Optional[bool] = None) -> None:
        """Mirror the hidden Tk anchor's position onto the GPU button."""
        gpu_btn = getattr(self, '_float_gpu_button', None)
        anchor = getattr(self, '_float', None)
        if gpu_btn is None or anchor is None:
            return
        try:
            x = int(anchor.winfo_x())
            y = int(anchor.winfo_y())
            w = int(getattr(self, '_fw', 0) or anchor.winfo_width() or 1)
            h = int(getattr(self, '_fh', 0) or anchor.winfo_height() or 1)
            gpu_btn.geometry(f'{w}x{h}+{x}+{y}')
        except Exception:
            pass
        try:
            if show is True:
                gpu_btn.deiconify()
            elif show is False:
                gpu_btn.withdraw()
        except Exception:
            pass

    def _set_float_alpha(self, alpha):
        """Set visible GPU trigger alpha; hidden Tk anchor stays invisible."""
        self._float_alpha = alpha
        gpu_btn = getattr(self, '_float_gpu_button', None)
        if gpu_btn is not None:
            try:
                gpu_btn.set_alpha(alpha)
            except Exception:
                pass

    # Round-64 note: the _sao_fx_panels + _sao_fx_after_id class attrs
    # were relocated to gui_modules.sao_gui_panel_fx_mixin alongside
    # the methods that use them (_attach_sao_panel_fx + _sao_fx_shared_tick).
    # MRO lookup still resolves them via SAOPlayerGUI's inheritance chain.

    def _start_float_breath(self):
        """idle 状态下轻微上下浮动 (模仿 SAO 菜单呼吸动画)"""
        if self._breath_active:
            return
        self._breath_active = True
        try:
            self._float.update_idletasks()
            self._breath_base_x = self._float.winfo_x()
            self._breath_base_y = self._float.winfo_y()
        except Exception:
            pass
        self._breath_t0 = time.time()
        self._breath_step()

    def _breath_step(self):
        if self._destroyed or not self._breath_active:
            return
        try:
            # v2.4.33: cython sin offsets — saves ~0.7us per frame at 60fps.
            new_dx, new_dy = _CY_UI.breath_offsets(time.time() - self._breath_t0)
            fx = self._breath_base_x + new_dx
            fy = self._breath_base_y + new_dy
            if self._float and self._float.winfo_exists():
                self._float.geometry(f'+{fx}+{fy}')
                self._sync_float_button_geometry(show=True)
            self.root.after(16, self._breath_step)
        except Exception:
            pass

    def _stop_float_breath(self):
        self._breath_active = False
        try:
            if self._float and self._float.winfo_exists():
                self._float.geometry(f'+{self._breath_base_x}+{self._breath_base_y}')
                self._sync_float_button_geometry(show=True)
        except Exception:
            pass

    def _attach_panel_float(self, panel, phase: float = 0.0, amp: float = 2.5):
        """给浮动面板附加轻微漂浮动画，且不再叠加额外 HUD 小条。"""
        key = id(panel)
        if key in self._panel_float_entries:
            return
        entry = {
            'panel': panel,
            'phase': float(phase or 0.0),
            'amp': float(amp or 0.0),
            't0': time.time(),
            'base_x': None,
            'base_y': None,
            'dx': 0,
            'dy': 0,
        }
        self._panel_float_entries[key] = entry
        panel._fdx = 0
        panel._fdy = 0

        def _on_destroy(event=None, _key=key):
            self._panel_float_entries.pop(_key, None)

        panel.bind('<Destroy>', _on_destroy, add='+')
        if self._panel_float_after_id is None:
            self._panel_float_shared_tick()

    @_probe.decorate('ui.panel_float.tick')
    def _panel_float_shared_tick(self):
        self._panel_float_after_id = None
        if self._destroyed:
            self._panel_float_entries.clear()
            return
        if not self._panel_float_entries:
            return

        now_abs = time.time()
        active_count = 0
        for key, entry in list(self._panel_float_entries.items()):
            panel = entry.get('panel')
            try:
                if panel is None or not panel.winfo_exists():
                    self._panel_float_entries.pop(key, None)
                    continue
                if not panel.winfo_viewable():
                    continue
                cur_x = int(panel.winfo_x())
                cur_y = int(panel.winfo_y())
            except Exception:
                self._panel_float_entries.pop(key, None)
                continue

            active_count += 1
            old_dx = int(entry.get('dx') or 0)
            old_dy = int(entry.get('dy') or 0)
            base_x = entry.get('base_x')
            base_y = entry.get('base_y')
            if base_x is None or base_y is None:
                base_x = cur_x - old_dx
                base_y = cur_y - old_dy
            else:
                expected_x = int(base_x) + old_dx
                expected_y = int(base_y) + old_dy
                if abs(cur_x - expected_x) > 2 or abs(cur_y - expected_y) > 2:
                    base_x = cur_x - old_dx
                    base_y = cur_y - old_dy

            t = now_abs - float(entry.get('t0') or now_abs)
            phase = float(entry.get('phase') or 0.0)
            amp = float(entry.get('amp') or 0.0)
            # v2.4.31: sin offsets in cython (libc.math.sin, no Python call).
            new_dx, new_dy = _CY_UI.panel_float_offsets(t, phase, amp)
            if new_dx != old_dx or new_dy != old_dy:
                try:
                    panel.geometry(f'+{int(base_x) + new_dx}+{int(base_y) + new_dy}')
                except Exception:
                    pass
            entry['base_x'] = int(base_x)
            entry['base_y'] = int(base_y)
            entry['dx'] = new_dx
            entry['dy'] = new_dy
            try:
                panel._fdx = new_dx
                panel._fdy = new_dy
            except Exception:
                pass

        _perf_gauge('ui.panel_float.active_panels', active_count)
        if self._panel_float_entries:
            try:
                delay_ms = 33 if active_count > 0 else 250
                self._panel_float_after_id = self.root.after(
                    delay_ms, self._panel_float_shared_tick)
            except Exception:
                self._panel_float_after_id = None

    def _update_float_display(self):
        """Refresh the platform float display."""
        self._refresh_float_layered()

    def _update_float_status(self):
        self._update_float_display()

    def _animate_float_to(self, x0, y0, x1, y1, ms=700):
        """将悬浮窗口从 (x0,y0) 平滑动画到 (x1,y1)"""
        steps = max(1, ms // 16)
        step = [0]
        def tick():
            if self._destroyed:
                return
            if not self._float.winfo_exists():
                return
            step[0] += 1
            t = min(1.0, step[0] / steps)
            et = ease_out(t)
            x = int(x0 + (x1 - x0) * et)
            y = int(y0 + (y1 - y0) * et)
            self._float.geometry(f'+{x}+{y}')
            self._sync_float_button_geometry(show=True)
            try:
                self._refresh_float_layered()
            except Exception:
                pass
            if t < 1.0:
                try:
                    self.root.after(16, tick)
                except Exception:
                    pass
        tick()

    def _raise_sao_menu_above_motion_blur(self):
        """Keep the already-open SAO popup above the async blur overlay."""
        try:
            menu = getattr(self, '_sao_menu', None)
            if menu is None or not getattr(menu, 'visible', False):
                return
            raise_to_top = getattr(menu, '_raise_to_top', None)
            if callable(raise_to_top):
                raise_to_top()
        except Exception:
            pass

    def _raise_fisheye_panels_above_motion_blur(self):
        """Keep visible Entity panels above the async closing blur overlay."""
        raise_panel = getattr(self, '_raise_panel_window', None)
        iter_panels = getattr(self, '_iter_fisheye_panels', None)
        is_visible = getattr(self, '_is_fisheye_panel_visible', None)
        if not callable(raise_panel) or not callable(iter_panels) or not callable(is_visible):
            return
        try:
            panels = list(iter_panels())
        except Exception:
            return
        for panel in panels:
            try:
                if is_visible(panel):
                    raise_panel(panel)
            except Exception:
                pass

    def _mark_motion_blur_active(self, ttl: float = 0.9) -> None:
        """Track async motion-blur teardown so dialogs wait for it."""
        try:
            count = int(getattr(self, '_motion_blur_active_count', 0) or 0)
        except Exception:
            count = 0
        self._motion_blur_active_count = max(0, count) + 1
        until = time.time() + max(0.05, float(ttl))
        try:
            prev = float(getattr(self, '_motion_blur_active_until', 0.0) or 0.0)
        except Exception:
            prev = 0.0
        self._motion_blur_active_until = max(prev, until)

    def _clear_motion_blur_active(self, settle: float = 0.08) -> None:
        try:
            count = int(getattr(self, '_motion_blur_active_count', 0) or 0)
        except Exception:
            count = 0
        count = max(0, count - 1)
        self._motion_blur_active_count = count
        if count <= 0:
            self._motion_blur_active_until = time.time() + max(0.0, float(settle))

    # ══════════════════════════════════════════════════════════════
    #  点击悬浮按钮 → 径向运动模糊闪现
    # ══════════════════════════════════════════════════════════════
    def _play_motion_blur(self, closing=False):
        """
        悬浮按钮点击时的径向运动模糊效果 (SAO 菜单展开/收起).

        以悬浮按钮为中心, 截取屏幕 → 径向缩放模糊 → 叠加层渐隐.
        • 后台线程: 截屏 + 径向模糊
        • 主线程: 显示结果 + 渐隐动画
        • WDA_EXCLUDEFROMCAPTURE: 防止鱼眼层捕获到此叠加层 (消除撕裂)
        • BILINEAR 缩放: 减少锯齿/马赛克感
        """
        try:
            from PIL import ImageGrab, Image, ImageTk, ImageFilter
        except ImportError:
            return

        self._mark_motion_blur_active(ttl=1.2)
        _blur_cleared = [False]

        def _clear_blur(settle: float = 0.08) -> None:
            if _blur_cleared[0]:
                return
            _blur_cleared[0] = True
            self._clear_motion_blur_active(settle=settle)

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()

        # 悬浮按钮中心作为模糊焦点
        try:
            fx = self._float.winfo_x() + self._fw // 2
            fy = self._float.winfo_y() + self._fh // 2
        except Exception:
            fx, fy = sw // 2, sh // 2

        def _build_and_show():
            """后台: 截屏 + 径向模糊 → 主线程显示."""
            # 截屏
            shot = None
            try:
                import mss as _mss_mod
                _sct = _mss_mod.mss()
                _mon = {"top": 0, "left": 0, "width": sw, "height": sh}
                s = _sct.grab(_mon)
                shot = Image.frombytes('RGB', s.size, s.rgb)
            except Exception:
                pass
            if shot is None:
                for _g in (
                    lambda: ImageGrab.grab(bbox=(0, 0, sw, sh), all_screens=True),
                    lambda: ImageGrab.grab(bbox=(0, 0, sw, sh)),
                    lambda: ImageGrab.grab(),
                ):
                    try:
                        shot = _g()
                        break
                    except Exception:
                        continue
            if shot is None:
                try:
                    self.root.after(0, _clear_blur)
                except Exception:
                    _clear_blur()
                return

            # 半分辨率处理 (1/2 而非 1/3, 提升清晰度)
            hw, hh = sw // 2, sh // 2
            small = shot.resize((hw, hh), Image.BILINEAR)
            cx, cy = fx / 2.0, fy / 2.0

            # 径向缩放模糊: 多次微缩放叠加
            import numpy as np
            acc = np.array(small, dtype=np.float32)
            n_layers = 5
            for i in range(1, n_layers + 1):
                scale = 1.0 + i * 0.012
                nw = int(hw * scale)
                nh = int(hh * scale)
                zoomed = small.resize((nw, nh), Image.BILINEAR)
                ox = int(cx * scale - cx)
                oy = int(cy * scale - cy)
                ox = max(0, min(ox, nw - hw))
                oy = max(0, min(oy, nh - hh))
                crop = zoomed.crop((ox, oy, ox + hw, oy + hh))
                acc += np.array(crop, dtype=np.float32)
            blurred = Image.fromarray(
                (acc / (n_layers + 1)).clip(0, 255).astype(np.uint8))

            from PIL import ImageEnhance
            if closing:
                blurred = ImageEnhance.Brightness(blurred).enhance(0.85)
            else:
                blurred = ImageEnhance.Brightness(blurred).enhance(1.12)

            full = blurred.resize((sw, sh), Image.BILINEAR)

            try:
                self.root.after(0, lambda img=full: _display(img))
            except Exception:
                _clear_blur()

        def _display(pil_img):
            """主线程: 显示模糊图 + 350ms ease-out 渐隐."""
            try:
                mb_ov = tk.Toplevel(self.root)
                mb_ov.overrideredirect(True)
                mb_ov.attributes('-topmost', True)
                mb_ov.attributes('-alpha', 0.0)
                mb_ov.geometry(f'{sw}x{sh}+0+0')
                cv = tk.Canvas(mb_ov, width=sw, height=sh,
                               highlightthickness=0, bg='black')
                cv.pack(fill=tk.BOTH, expand=True)
                photo = ImageTk.PhotoImage(pil_img)
                cv.create_image(0, 0, image=photo, anchor='nw')
                cv._photo = photo

                # WDA_EXCLUDEFROMCAPTURE: 鱼眼截屏不会捕获到此 overlay
                try:
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
                    _SWP_NOOWNERZORDER = 0x0200
                    mb_ov.update_idletasks()
                    hwnd = _u32.GetParent(mb_ov.winfo_id()) or mb_ov.winfo_id()
                    ex = _u32.GetWindowLongPtrW(_ct.c_void_p(hwnd), _GWL_EXSTYLE)
                    _u32.SetWindowLongPtrW(
                        _ct.c_void_p(hwnd), _GWL_EXSTYLE,
                        ex | _WS_EX_LAYERED | _WS_EX_TRANSPARENT
                        | _WS_EX_TOOLWINDOW | _WS_EX_NOACTIVATE,
                    )
                    _u32.SetWindowPos(
                        _ct.c_void_p(hwnd), _ct.c_void_p(_HWND_TOPMOST),
                        0, 0, 0, 0,
                        _SWP_NOMOVE | _SWP_NOSIZE
                        | _SWP_NOACTIVATE | _SWP_NOOWNERZORDER,
                    )
                    _u32.SetWindowDisplayAffinity(hwnd, 0x00000011)
                except Exception:
                    pass
                if not closing:
                    # This full-screen topmost blur appears after
                    # SAOPopUpMenu.open() because the image is built on a
                    # worker. Re-raise the popup so it does not flash-hidden
                    # until the blur fades out.
                    self._raise_sao_menu_above_motion_blur()
                    try:
                        self.root.after(32, self._raise_sao_menu_above_motion_blur)
                        self.root.after(96, self._raise_sao_menu_above_motion_blur)
                    except Exception:
                        pass
                else:
                    self._raise_fisheye_panels_above_motion_blur()
                    try:
                        self.root.after(32, self._raise_fisheye_panels_above_motion_blur)
                        self.root.after(96, self._raise_fisheye_panels_above_motion_blur)
                        self.root.after(180, self._raise_fisheye_panels_above_motion_blur)
                    except Exception:
                        pass
            except Exception:
                _clear_blur()
                return

            # 快速渐入 (50ms) → 缓慢渐隐 (350ms), 消除突然出现的闪烁感
            _t0 = time.time()
            _fadein_dur = 0.05
            _fadeout_dur = 0.35
            _peak = 0.72

            def _mblur_anim():
                dt = time.time() - _t0
                if dt < _fadein_dur:
                    # 渐入阶段
                    a = _peak * (dt / _fadein_dur)
                elif dt < _fadein_dur + _fadeout_dur:
                    # 渐隐阶段
                    t = (dt - _fadein_dur) / _fadeout_dur
                    a = _peak * (1.0 - t ** 0.6)
                else:
                    try: mb_ov.destroy()
                    except Exception: pass
                    _clear_blur()
                    return
                try: mb_ov.attributes('-alpha', max(0.0, a))
                except Exception: pass
                try: mb_ov.after(16, _mblur_anim)
                except Exception: _clear_blur()

            mb_ov.after(1, _mblur_anim)

        import threading as _th
        _th.Thread(target=_build_and_show, daemon=True).start()

    # ══════════════════════════════════════════════════════════════
    #  持久鱼眼叠加层 (菜单开启时常驻, 关闭时销毁)
    # ══════════════════════════════════════════════════════════════
    def _update_float_title(self):
        """Refresh the platform HUD label."""
        try:
            self._refresh_float_layered()
        except Exception:
            pass

    # ══════════════════════════════════════════════
    #  快捷键
    # ══════════════════════════════════════════════
