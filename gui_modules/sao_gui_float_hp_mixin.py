# -*- coding: utf-8 -*-
"""
SAOPlayerGUIFloatHpMixin — fourteenth mixin extracted from
SAOPlayerGUI (round 56 of the sao_gui split refactor). 20 methods,
~409 lines.

The two clusters bundled together because they're both "the
peripheral chrome around the SAO HUD" — the floating button and
the HP overlay context menu + UI hooks:

HP overlay context handlers (6 methods, ~64 lines):
  * _refresh_hp_layered — deprecated 75%-screen ULW compat shim
  * _reset_sta_offline_state — clear stamina-offline cache
  * _should_show_sta_offline — predicate (cython-backed via
    inline calc)
  * _hp_overlay_on_click — left-click → open SAO menu
  * _hp_overlay_restore_position — context menu: restore default xy
  * _hp_overlay_hide — context menu: hide-panel branch (settings
    persist + alert pop-up)

Float button + breath + animation (14 methods, ~345 lines):
  * _build_float_hud_items / _animate_float_hud — small HUD items
    on the float button itself
  * _set_float_alpha — Win32 SetLayeredWindowAttributes wrapper
  * _start_float_breath / _breath_step / _stop_float_breath —
    idle-state ~2 Hz vertical sine wobble
  * _attach_panel_float / _panel_float_shared_tick — wire a
    panel into the shared 30 fps panel-float scheduler
  * _update_float_display / _update_float_status /
    _update_float_fname / _update_float_title — small label
    refresh helpers
  * _animate_float_to — drag-end snap animation
  * _play_motion_blur (146 lines — biggest in this cluster) —
    radial blur effect on SAO menu open/close (background-threaded
    screen grab + radial blur + main-thread fade)

Required SAOPlayerGUI attrs:
  * self._float, self._fw, self._fh, self._float_hp_name_lbl
  * self._breath_active, self._breath_after_id, self._breath_base_x,
    self._breath_base_y, self._breath_phase, self._breath_amp
  * self._panel_float_after_id, self._panel_float_attached
  * self._hp_overlay, self._hp_ov_visible
  * self._alert_overlay, self._sao_menu, self.root, self.settings

Required SAOPlayerGUI methods (via MRO):
  * _toggle_sao_menu (Menu mixin)
  * _show_entity_alert (Dialogs mixin)
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Optional

from perf_probe import probe as _probe


class SAOPlayerGUIFloatHpMixin:
    """Mixin bundling HP overlay context handlers + float button +
    breath + motion blur + panel-float animations."""

    def _build_float_hud_items(self):
        """(ULW 模式下 HUD 已统一由 PIL 渲染, 此方法保留接口兼容)"""
        pass

    def _refresh_hp_layered(self):
        """(deprecated) 旧 75%屏宽 HP ULW 已移除, 此方法为兼容占位。"""
        return

    def _set_float_alpha(self, alpha):
        """(deprecated) _float 现为全透明点击锚点, 不再需要 alpha。"""
        self._float_alpha = alpha

    def _animate_float_hud(self):
        """(deprecated) 旧 30fps HP 重绘循环已移除 (HpOverlay 自管帧率)。"""
        return

    # Round-64 note: the _sao_fx_panels + _sao_fx_after_id class attrs
    # were relocated to gui_modules.sao_gui_panel_fx_mixin alongside
    # the methods that use them (_attach_sao_panel_fx + _sao_fx_shared_tick).
    # MRO lookup still resolves them via SAOPlayerGUI's inheritance chain.

    def _reset_sta_offline_state(self):
        self._sta_offline_armed = False
        try:
            if self._hp_overlay and getattr(self, '_hp_ov_visible', True):
                self._hp_overlay.set_sta_offline(False)
        except Exception:
            pass

    def _should_show_sta_offline(self, gs) -> bool:
        if gs is None:
            return False
        # v2.2.23: packet-driven STA wins. When the packet bridge has a
        # valid stamina_max, the bar IS in the game world — even if the
        # vision capture transiently fails (skill FX overlays the STA
        # bar, capture frame goes blank under heavy load, etc.). The
        # previous logic reported offline based purely on vision, which
        # caused the entire HP panel to auto-hide after combat bursts
        # even though packets kept streaming valid STA values.
        try:
            if int(getattr(gs, 'stamina_max', 0) or 0) > 0:
                return False
        except Exception:
            pass
        # No packet STA available — fall back to vision-driven offline.
        return bool(getattr(gs, 'stamina_offline', False))

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
            self.root.after(16, self._breath_step)
        except Exception:
            pass

    def _stop_float_breath(self):
        self._breath_active = False
        try:
            if self._float and self._float.winfo_exists():
                self._float.geometry(f'+{self._breath_base_x}+{self._breath_base_y}')
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
        """更新悬浮 HP 组件 — 由 _render_hp_dynamic 统一处理，仅触发刷新。"""
        self._refresh_hp_layered()

    def _update_float_status(self):
        self._update_float_display()

    def _update_float_fname(self, name=''):
        """HP 组件风格: 无文件名显示, 保留接口兼容"""
        pass

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
            try:
                self._refresh_hp_layered()
            except Exception:
                pass
            if t < 1.0:
                try:
                    self.root.after(16, tick)
                except Exception:
                    pass
        tick()

    def _hp_overlay_on_click(self):
        """Left-click on HP panel: open the SAO radial menu (web parity)."""
        try:
            self._toggle_sao_menu(allow_close=True)
        except Exception:
            pass

    def _hp_overlay_restore_position(self):
        ov = getattr(self, '_hp_overlay', None)
        if ov is not None:
            try:
                ov.restore_position()
            except Exception:
                pass

    def _hp_overlay_hide(self):
        ov = getattr(self, '_hp_overlay', None)
        if ov is not None:
            try:
                ov.hide()
                self._hp_ov_visible = False
                try:
                    self.settings.set('hp_ov_enabled', False)
                    save = getattr(self.settings, 'save', None)
                    if callable(save):
                        save()
                except Exception:
                    pass
            except Exception:
                pass

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
                pass

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
                    mb_ov.update_idletasks()
                    hwnd = _u32.GetParent(mb_ov.winfo_id()) or mb_ov.winfo_id()
                    _u32.SetWindowDisplayAffinity(hwnd, 0x00000011)
                except Exception:
                    pass
            except Exception:
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
                    return
                try: mb_ov.attributes('-alpha', max(0.0, a))
                except Exception: pass
                try: mb_ov.after(16, _mblur_anim)
                except Exception: pass

            mb_ov.after(1, _mblur_anim)

        import threading as _th
        _th.Thread(target=_build_and_show, daemon=True).start()

    # ══════════════════════════════════════════════════════════════
    #  持久鱼眼叠加层 (菜单开启时常驻, 关闭时销毁)
    # ══════════════════════════════════════════════════════════════
    def _update_float_title(self):
        """更新 HP 组件的用户名"""
        try:
            name = self._username if self._username else 'Player'
            if len(name) > 8:
                name = name[:7] + '…'
            self._hp_display_name = name
            self._refresh_hp_layered()
        except Exception:
            pass

    # ══════════════════════════════════════════════
    #  快捷键
    # ══════════════════════════════════════════════
