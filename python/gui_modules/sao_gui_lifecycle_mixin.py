# -*- coding: utf-8 -*-
# SAOPlayerGUILifecycleMixin — sixteenth mixin extracted from
# SAOPlayerGUI (round 62 of the sao_gui split refactor). 7 methods,
# ~324 lines.
#
# The application teardown + restore-on-startup helpers — the
# ordered shutdown sequence that brings down engines, panels,
# overlays, hotkeys, and the float button in the right order so
# no Tk after-id leaks past mainloop quit.
#
# Methods:
# * _restore_panels (13) — on startup, re-open panels that were
# visible last session (respects _panels_hidden flag).
# * _cleanup_entry_overlay (23) — destroys the SAO link-start
# entry-animation overlay.
# * _cleanup_exit_overlay (24) — destroys the exit-animation
# overlay.
# * _finalize_close (128) — the ordered destroy sequence:
# 1. set _destroyed/_close_finalized, stop breath/lift loops
# 2. cancel all root.after IDs (panel float, menu refresh,
# shared fx tick on the class)
# 3. remove updater listener
# 4. unbind SAOHotkeyManager + optional plugin state manager
# 5. stop fisheye overlay
# 6. close + destroy SAO menu overlay
# 7. stop recognition engines + persist optional plugin cache
# 8. destroy floating panels and ULW overlays
# 9. quit root.mainloop()
# * _run_exit_animation (124) — confirm + fade-out + exit overlay
# + scheduled hard-exit/finalize after the animation.
# * _on_close (3) — top-level handler: delegate to
# _run_exit_animation.
#
# Round-62 fix: references to `SAOPlayerGUI._sao_fx_after_id` (the
# shared panel-fx class attr) are rewritten to `type(self)._sao_fx_after_id`
# so the mixin doesn't need to import the not-yet-defined SAOPlayerGUI
# class. MRO + class-attr lookup keeps the semantics identical.
#
# Required SAOPlayerGUI attrs:
# * self._destroyed, self._close_finalized, self._exit_animating,
# self._breath_active, self._lift_loop_active, self._panels_hidden
# * self.root, self.settings, self._float, self._sao_menu,
# self._cfg_settings_ref, self._updater_mgr,
# self._update_listener, self._update_listener_installed,
# self._hotkey_mgr, self._cache_loop_stop, self._after_shutdown
# * self._update_panel, self._fisheye_ov
# * Class attr: SAOPlayerGUI._sao_fx_after_id (accessed via type(self))
#
# Required SAOPlayerGUI methods (via MRO):
# * _persist_entity_menu_state (Menu mixin)
# * Optional plugin cache/recognition hooks when installed
# * _stop_fisheye_overlay (Fisheye mixin)
# * _toggle_status_panel (StatusUpdater mixin)

from __future__ import annotations

import os
import time
from typing import Any, Optional

from utils.sao_sound import play_sound
from sao_theme import ease_out, ease_in_out


class SAOPlayerGUILifecycleMixin:
    # Mixin bundling teardown + restore-on-startup helpers.

    def _restore_panels(self):
        # Restore platform-owned floating panels only.
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
        # Stop producers and fully tear down every HWND/compositor owner
        # before asking the helper to clean kernel state.  A helper teardown
        # while overlay/_dc work is still being produced can otherwise race
        # a late mutation against unload.
        plugins_stopped = False
        try:
            from act_platform.runtime import shutdown_act_plugin_manager
            plugins_stopped = bool(shutdown_act_plugin_manager(self))
        except Exception:
            plugins_stopped = False
        if not plugins_stopped:
            try:
                print('[SAO] hard exit blocked: plugin workers still active',
                      flush=True)
            except Exception:
                pass
            return
        try:
            from utils.sao_sound import unload_sao_fonts
            unload_sao_fonts()
        except Exception:
            pass
        if not self._finalize_close(quit_root=False):
            # ``UnifiedOverlay.stop`` preserves a live thread on timeout.
            # Do not pretend shutdown succeeded and do not pull the helper
            # out from underneath that still-live owner.
            try:
                print('[SAO] hard exit blocked: compositor did not stop',
                      flush=True)
            except Exception:
                pass
            return
        try:
            from mem_probe.process import _wait_for_target_leases_closed
            producers_stopped = _wait_for_target_leases_closed(timeout=20.0)
        except Exception:
            producers_stopped = False
        if not producers_stopped:
            try:
                print('[SAO] hard exit blocked: memory consumers still active',
                      flush=True)
            except Exception:
                pass
            return
        # Wait for the rt_io helper subprocess to finish its kernel-side
        # teardown before we let os._exit(0) yank the interpreter. The old
        # path bypassed atexit → helper got killed 2 s later mid-teardown →
        # kernel state (Ob callback pool, DPC timer, UC-tainted PFNs) left
        # partially set up → next launch bugchecks 0x1A_2101 in
        # MiProcessLoaderEntry when MI validates the newly-loaded driver
        # image against the leftover PFN cache-attribute state.
        #
        # We show a small modal so the user knows the ~200 ms — 15 s wait
        # is intentional, then explicitly call _stop_helper on a worker
        # thread and pump Tk events while it drains.
        if not self._await_helper_shutdown_ui():
            try:
                print('[SAO] hard exit blocked: helper cleanup unconfirmed',
                      flush=True)
            except Exception:
                pass
            return
        os._exit(0)

    def _await_helper_shutdown_ui(self) -> bool:
        # Blocking wait with visible progress. Never raises.
        try:
            from mem_probe import rt_io_proxy
        except Exception:
            return False
        # A dead child is not proof that backend cleanup committed: it may have
        # crashed in FAILED state before sending CleanupReport/ACK.  Always ask
        # the session owner for its confirmed stop result, including STOPPED.
        # Modal Tk toplevel with a simple status label. Uses grab_set so
        # the user can't restart the app mid-teardown.
        try:
            import tkinter as tk
            top = tk.Toplevel(self.root)
            top.title('退出中')
            top.transient(self.root)
            top.resizable(False, False)
            try:
                top.attributes('-topmost', True)
            except Exception:
                pass
            frm = tk.Frame(top, padx=24, pady=18)
            frm.pack()
            tk.Label(frm, text='正在清理驱动状态，请稍候…',
                     font=('Microsoft YaHei UI', 11)).pack(pady=(0, 6))
            status = tk.Label(frm, text='等待 helper 退出',
                              font=('Microsoft YaHei UI', 9), fg='#666')
            status.pack()
            # Center over the main window if possible.
            try:
                self.root.update_idletasks()
                rx, ry = self.root.winfo_rootx(), self.root.winfo_rooty()
                rw, rh = self.root.winfo_width(), self.root.winfo_height()
                top.update_idletasks()
                tw, th = top.winfo_width(), top.winfo_height()
                top.geometry(f'+{rx + (rw - tw) // 2}+{ry + (rh - th) // 2}')
            except Exception:
                pass
            try:
                top.grab_set()
            except Exception:
                pass
        except Exception:
            top = None
            status = None
        # Run the actual stop in a background thread so we can pump Tk.
        import threading, time
        done = threading.Event()
        confirmed = [False]

        def _worker():
            try:
                result = rt_io_proxy._stop_helper(wait_seconds=15.0)
                if isinstance(result, bool):
                    confirmed[0] = result
                elif isinstance(result, dict):
                    confirmed[0] = bool(result.get('confirmed', False))
                else:
                    confirmed[0] = bool(
                        getattr(result, 'confirmed', False))
            except Exception:
                confirmed[0] = False
            done.set()

        threading.Thread(target=_worker, daemon=True,
                         name='helper-graceful-stop').start()
        t0 = time.time()
        while not done.is_set():
            elapsed = time.time() - t0
            if elapsed > 20.0:  # hard ceiling above _stop_helper's own 15s
                break
            if status is not None:
                try:
                    status.configure(text=f'等待 helper 退出（{elapsed:.1f}s）')
                except Exception:
                    pass
            if top is not None:
                try:
                    self.root.update()
                except Exception:
                    pass
            time.sleep(0.05)
        if top is not None:
            try:
                top.grab_release()
            except Exception:
                pass
            try:
                top.destroy()
            except Exception:
                pass
        return bool(done.is_set() and confirmed[0]
                    and not getattr(
                        rt_io_proxy, 'helper_alive', lambda: True)())

    def _stop_overlay_runtime(self) -> bool:
        webviews_stopped = False
        try:
            from render.webview_proxy import stop_all_proxies
            webviews_stopped = bool(stop_all_proxies())
        except Exception:
            webviews_stopped = False
        if not webviews_stopped:
            self._compositor_stop_confirmed = False
            return False
        mirrors_stopped = False
        try:
            from render.tk_mirror import stop_all_mirrors
            mirrors_stopped = bool(stop_all_mirrors())
        except Exception:
            mirrors_stopped = False
        if not mirrors_stopped:
            self._compositor_stop_confirmed = False
            return False
        compositor_stopped = True
        try:
            from render import gpu_overlay_window as _gow
            instance = getattr(_gow, '_unified_overlay_instance', None)
            if instance is not None:
                compositor_stopped = bool(instance.stop())
        except Exception:
            compositor_stopped = False
        self._compositor_stop_confirmed = compositor_stopped
        return compositor_stopped

    def _finalize_close(self, *, quit_root: bool = True):
        if self._close_finalized:
            stopped = self._stop_overlay_runtime()
            if stopped and quit_root:
                try:
                    self.root.quit()
                except Exception:
                    pass
            return stopped
        try:
            from act_platform.runtime import shutdown_act_plugin_manager
            if not shutdown_act_plugin_manager(self):
                return False
        except Exception:
            return False
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
        release_plugin_lifecycle = getattr(self, '_release_plugin_lifecycle_subscription', None)
        if callable(release_plugin_lifecycle):
            release_plugin_lifecycle()
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
        panels_stopped = True
        for attr_name in list(vars(self)):
            if attr_name.endswith('_overlay') or attr_name.endswith('_panel'):
                obj = getattr(self, attr_name, None)
                if obj is not None:
                    destroyed = True
                    try:
                        destroy = getattr(obj, 'destroy', None)
                        if callable(destroy):
                            destroyed = destroy() is not False
                    except Exception:
                        destroyed = False
                    if destroyed:
                        try:
                            setattr(self, attr_name, None)
                        except Exception:
                            pass
                    else:
                        panels_stopped = False
        if not panels_stopped:
            return False
        if getattr(self, '_ai_editor_panel', None):
            try:
                self._ai_editor_panel.destroy()
            except Exception:
                pass
        self._ai_editor_panel = None
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
        # Stop unified overlay compositor.  The instance is authoritative;
        # mode can already be false during a failed/prestart transition.
        compositor_stopped = self._stop_overlay_runtime()
        if not compositor_stopped:
            return False
        self._close_finalized = True
        if quit_root:
            try:
                self.root.quit()  # 退出 mainloop，由 run() 负责 destroy
            except Exception:
                pass
        return True

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
