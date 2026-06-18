# -*- coding: utf-8 -*-
"""
SAOPlayerGUILifecycleMixin — sixteenth mixin extracted from
SAOPlayerGUI (round 62 of the sao_gui split refactor). 7 methods,
~324 lines.

The application teardown + restore-on-startup helpers — the
ordered shutdown sequence that brings down engines, panels,
overlays, hotkeys, and the float button in the right order so
no Tk after-id leaks past mainloop quit.

Methods:
    * _destroy_float_alpha_windows (9) — destroys legacy float alpha
        strip windows (compat path; usually no-op).
  * _restore_panels (13) — on startup, re-open panels that were
    visible last session (respects _panels_hidden flag).
  * _cleanup_entry_overlay (23) — destroys the SAO link-start
    entry-animation overlay.
  * _cleanup_exit_overlay (24) — destroys the exit-animation
    overlay.
  * _finalize_close (128) — the ordered destroy sequence:
      1. set _destroyed/_close_finalized, stop breath/lift loops
      2. cancel all root.after IDs (panel float, menu refresh,
         shared fx tick on the class)
      3. remove updater listener
      4. unbind SAOHotkeyManager + optional plugin state manager
      5. stop fisheye overlay
      6. close + destroy SAO menu overlay
      7. stop recognition engines + persist optional plugin cache
      8. destroy floating panels and ULW overlays
      9. quit root.mainloop()
  * _run_exit_animation (124) — confirm + fade-out + exit overlay
    + scheduled hard-exit/finalize after the animation.
  * _on_close (3) — top-level handler: delegate to
    _run_exit_animation.

Round-62 fix: references to `SAOPlayerGUI._sao_fx_after_id` (the
shared panel-fx class attr) are rewritten to `type(self)._sao_fx_after_id`
so the mixin doesn't need to import the not-yet-defined SAOPlayerGUI
class. MRO + class-attr lookup keeps the semantics identical.

Required SAOPlayerGUI attrs:
  * self._destroyed, self._close_finalized, self._exit_animating,
    self._breath_active, self._lift_loop_active, self._panels_hidden
  * self.root, self.settings, self._float, self._sao_menu,
    self._cfg_settings_ref, self._updater_mgr,
    self._update_listener, self._update_listener_installed,
    self._hotkey_mgr, self._cache_loop_stop, self._after_shutdown
    * self._update_panel, self._fisheye_ov
  * Class attr: SAOPlayerGUI._sao_fx_after_id (accessed via type(self))

Required SAOPlayerGUI methods (via MRO):
  * _persist_entity_menu_state (Menu mixin)
  * Optional plugin cache/recognition hooks when installed
  * _stop_fisheye_overlay (Fisheye mixin)
  * _toggle_status_panel (StatusUpdater mixin)
"""

from __future__ import annotations

import os
import time
from typing import Any, Optional

from utils.sao_sound import play_sound
from sao_theme import ease_out, ease_in_out


class SAOPlayerGUILifecycleMixin:
    """Mixin bundling teardown + restore-on-startup helpers."""

    def _destroy_float_alpha_windows(self):
        for item in getattr(self, '_float_alpha_windows', []):
            try:
                item['win'].destroy()
            except Exception:
                pass
        self._float_alpha_windows = []
        self._float_alpha_photos = []

    def _restore_panels(self):
        """Restore platform-owned floating panels only."""
        if self._panels_hidden:
            return

    def _cleanup_exit_overlay(self):
        ov = getattr(self, '_exit_overlay', None)
        if not ov:
            return
        try:
            gpu = ov.get('gpu_transition')
            destroy = getattr(gpu, 'destroy', None)
            if callable(destroy):
                destroy()
        except Exception:
            pass
        try:
            gl = ov.get('gl')
            if gl:
                for key in ('pulse_tex', 'pulse_fbo', 'pulse_prog', 'pulse_vao', 'ctx'):
                    try:
                        obj = gl.get(key)
                        if obj is not None:
                            obj.release()
                    except Exception:
                        pass
        except Exception:
            pass
        try:
            win = ov.get('win')
            if win and win.winfo_exists():
                win.destroy()
        except Exception:
            pass
        self._exit_overlay = None

    def _cleanup_entry_overlay(self):
        ov = getattr(self, '_entry_overlay', None)
        if not ov:
            return
        try:
            gpu = ov.get('gpu_transition')
            destroy = getattr(gpu, 'destroy', None)
            if callable(destroy):
                destroy()
        except Exception:
            pass
        try:
            gl = ov.get('gl') or {}
            for key in ('boot_fbo', 'boot_tex', 'boot_vao', 'boot_prog', 'ctx'):
                obj = gl.get(key)
                if obj is not None:
                    try:
                        obj.release()
                    except Exception:
                        pass
        except Exception:
            pass
        try:
            win = ov.get('win')
            if win and win.winfo_exists():
                win.destroy()
        except Exception:
            pass
        self._entry_overlay = None

    def _hard_exit_process(self) -> None:
        os._exit(0)

    def _finalize_close(self):
        if self._close_finalized:
            return
        self._close_finalized = True
        self._destroyed = True
        self._breath_active = False
        self._lift_loop_active = False
        # ── Cancel all global after() IDs ──
        for _aid_attr in ('_panel_float_after_id', '_menu_refresh_after_id'):
            _aid = getattr(self, _aid_attr, None)
            if _aid is not None:
                try:
                    self.root.after_cancel(_aid)
                except Exception:
                    pass
                setattr(self, _aid_attr, None)
        # Cancel shared HUD fx tick (class-level)
        _fx_aid = getattr(type(self), '_sao_fx_after_id', None)
        if _fx_aid is not None:
            try:
                self.root.after_cancel(_fx_aid)
            except Exception:
                pass
            type(self)._sao_fx_after_id = None
        # ── Remove updater listener ──
        _umgr = getattr(self, '_updater_mgr', None)
        _ulistener = getattr(self, '_update_listener', None)
        if _umgr is not None and _ulistener is not None:
            try:
                _umgr.remove_listener(_ulistener)
            except Exception:
                pass
            self._updater_mgr = None
            self._update_listener = None
            self._update_listener_installed = False
        self._cleanup_entry_overlay()
        # Keep the exit overlay alive while child GPU windows are torn
        # down. If it is destroyed first, plugin layered windows can
        # briefly expose a black compositor frame after the animation.
        hotkey_mgr = getattr(self, '_hotkey_mgr', None)
        cleanup_hotkeys = getattr(hotkey_mgr, 'cleanup', None)
        if callable(cleanup_hotkeys):
            cleanup_hotkeys()
        try:
            state_mgr = getattr(self, '_state_mgr', None)
            if state_mgr:
                state_mgr.unsubscribe(self._on_game_state_update)
        except Exception:
            pass
        self._stop_fisheye_overlay(wait=True)
        try:
            if self._sao_menu is not None:
                self._sao_menu.unbind_events()
                self._sao_menu.force_destroy_overlay()
        except Exception:
            pass
        # 停止识别引擎
        self._recognition_active = False
        self._cache_loop_stop.set()
        stop_recognition = getattr(self, '_stop_recognition_engines', None)
        if callable(stop_recognition):
            stop_recognition()
        # 保存缓存
        state_mgr = getattr(self, '_state_mgr', None)
        cfg_ref = getattr(self, '_cfg_settings_ref', None)
        if state_mgr and cfg_ref:
            try:
                self._persist_entity_menu_state(save_now=False)
                self._persist_cached_identity_state(save_now=False)
                state_mgr.save_cache(cfg_ref)
            except Exception:
                pass
        elif cfg_ref:
            try:
                self._persist_entity_menu_state(save_now=False)
                self._persist_cached_identity_state(save_now=True)
            except Exception:
                pass
        # 销毁所有浮动面板
        for panel in [getattr(self, '_status_panel', None), getattr(self, '_update_panel', None)]:
            try:
                if panel and panel.winfo_exists():
                    panel.destroy()
            except Exception:
                pass
        # 销毁所有注册的覆盖层 + 面板 (动态遍历, 不硬编码名称)
        for attr_name in list(vars(self)):
            if attr_name.endswith('_overlay') or attr_name.endswith('_panel'):
                obj = getattr(self, attr_name, None)
                if obj is not None:
                    try:
                        destroy = getattr(obj, 'destroy', None)
                        if callable(destroy):
                            destroy()
                    except Exception:
                        pass
                    try:
                        setattr(self, attr_name, None)
                    except Exception:
                        pass
        if getattr(self, '_ai_editor_panel', None):
            try:
                self._ai_editor_panel.destroy()
            except Exception:
                pass
        self._ai_editor_panel = None
        self._destroy_float_alpha_windows()
        try:
            gpu_btn = getattr(self, '_float_gpu_button', None)
            if gpu_btn is not None:
                gpu_btn.destroy()
                self._float_gpu_button = None
        except Exception:
            pass
        try:
            if self._float and self._float.winfo_exists():
                self._float.destroy()
        except Exception:
            pass
        self._cleanup_exit_overlay()
        try:
            self.root.quit()  # 退出 mainloop，由 run() 负责 destroy
        except Exception:
            pass

    def _run_exit_animation(
            self, after_shutdown=None, mode='exit', target_label=None,
            hard_exit: Optional[bool] = None):
        if self._close_finalized or self._exit_animating:
            return
        self._exit_animating = True
        self._destroyed = True
        self._breath_active = False
        self._lift_loop_active = False
        try:
            play_sound('menu_close')
        except Exception:
            pass
        try:
            self._play_motion_blur(closing=True)
        except Exception:
            pass
        try:
            if self._sao_menu is not None and self._sao_menu.visible:
                self._sao_menu.prepare_external_fade()
        except Exception:
            pass
        # Layered/GPU overlays cannot be alpha-faded by _collect_exit_windows().
        # Ask every registered overlay to self-fade/hide if it supports that.
        overlays = []
        for attr_name, value in vars(self).items():
            if attr_name.endswith('_overlay') and value is not None:
                overlays.append(value)
        for ov in overlays:
            try:
                if ov is None:
                    continue
                fade_out = getattr(ov, 'fade_out', None)
                if callable(fade_out):
                    fade_out()
                    continue
                hide = getattr(ov, 'hide', None)
                if callable(hide):
                    hide()
            except Exception:
                pass

        wins = self._collect_exit_windows()
        self._create_exit_overlay(mode=mode, target_label=target_label)

        def _exit_overlay_uses_gpu() -> bool:
            try:
                ov = getattr(self, '_exit_overlay', None)
                return bool(ov and ov.get('gpu_transition') is not None)
            except Exception:
                return False

        use_hard_exit = (mode == 'exit') if hard_exit is None else bool(hard_exit)

        def _complete_close():
            if use_hard_exit:
                if after_shutdown:
                    try:
                        after_shutdown()
                    except Exception:
                        pass
                self._hard_exit_process()
                return
            self._finalize_close()
            if after_shutdown:
                try:
                    after_shutdown()
                except Exception:
                    pass

        def _finish(final_frame_drawn: bool = False):
            if _exit_overlay_uses_gpu():
                try:
                    if not final_frame_drawn:
                        self._draw_exit_overlay(1.0)
                except Exception:
                    pass
                try:
                    self.root.after(80, _complete_close)
                    return
                except Exception:
                    pass
            _complete_close()

        if not wins:
            self._draw_exit_overlay(1.0)
            _finish(final_frame_drawn=True)
            return

        t0 = time.time()
        stage1 = 0.34
        stage2 = 0.82
        duration = stage1 + stage2

        def _step():
            if self._close_finalized:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / duration)
            self._draw_exit_overlay(t)
            for item in wins:
                try:
                    win = item['win']
                    if not win.winfo_exists():
                        continue
                    if elapsed < stage1:
                        hold = ease_out(min(1.0, elapsed / stage1))
                        new_alpha = item['alpha'] * (1.0 - 0.10 * hold)
                        if item.get('movable'):
                            dx = int(item['ux'] * item['travel'] * 0.06 * hold)
                            dy = int(item['uy'] * item['travel'] * 0.06 * hold)
                            if item.get('role') == 'float':
                                dy -= int(6 * hold)
                            try:
                                win.geometry(f'+{item["x"] + dx}+{item["y"] + dy}')
                                if item.get('role') == 'float':
                                    self._sync_float_button_geometry(show=True)
                            except Exception:
                                pass
                    else:
                        local = min(1.0, max(0.0, (elapsed - stage1 - item['delay']) / max(0.001, item['duration'])))
                        fade = ease_in_out(local)
                        base_alpha = item['alpha'] * 0.90
                        new_alpha = max(0.0, base_alpha * (1.0 - fade))
                        if item.get('movable'):
                            dx = int(item['ux'] * item['travel'] * (0.06 + 0.94 * fade))
                            dy = int(item['uy'] * item['travel'] * (0.06 + 0.94 * fade))
                            if item.get('role') == 'float':
                                dy -= int(14 + 18 * fade)
                            try:
                                win.geometry(f'+{item["x"] + dx}+{item["y"] + dy}')
                                if item.get('role') == 'float':
                                    self._sync_float_button_geometry(show=True)
                            except Exception:
                                pass
                    if item.get('ulw'):
                        self._set_float_alpha(new_alpha)
                    else:
                        win.attributes('-alpha', new_alpha)
                except Exception:
                    pass
            if elapsed < duration:
                try:
                    self.root.after(16, _step)
                except Exception:
                    _finish()
            else:
                _finish(final_frame_drawn=True)

        try:
            self.root.after(1, _step)
        except Exception:
            _finish()

    def _on_close(self):
        self._run_exit_animation(mode='exit', target_label='Desktop')
