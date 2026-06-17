# -*- coding: utf-8 -*-
"""
SAOPlayerGUIDialogsMixin — eleventh mixin extracted from
SAOPlayerGUI (round 52 of the sao_gui split refactor). 7 methods,
~197 lines.

A small grab-bag mixin for SAOPlayerGUI helpers that build modal
dialogs and the right-click HP overlay context menu. Game-specific
left widgets and profile editors are installed by plugins.

Methods:
  * _hp_overlay_on_menu(x_root, y_root) — right-click context menu
    for the floating HP panel: SAO Menu / recognition toggle /
    restore-position / hide-panel / exit.
  * _show_welcome_then_menu — first-launch flow: profile dialog
    first, SAO menu second.
  * _show_entity_alert(title, message='', display_time=5.0) —
    convenience around self._alert_overlay.show_alert with a
    print() fallback.
  * _switch_to_webview_ui — confirm dialog + exit-animation + fresh
    process restart into WebView UI.
  * _show_about — SAO 'about' info dialog with updater state hint.
  * _edit_profile — plugin-owned profile editor hook.

Required SAOPlayerGUI attrs:
  * self._sao_menu
  * self._sta_hp, self._sta_sta
  * self._alert_overlay, self._hp_overlay, self._recognition_active
  * self._float, self.root, self.settings, self._cfg_settings_ref
  * self._profile_dialog_ref, self._profile_dialog_pending

Required SAOPlayerGUI methods (via MRO):
  * _get_setting, _set_setting (SAOPlayerGUI)
  * _toggle_sao_menu (Menu mixin)
  * _toggle_recognition_menu (Panels mixin)
  * _hp_overlay_on_click, _hp_overlay_restore_position,
    _hp_overlay_hide, _on_close (SAOPlayerGUI)
  * _update_float_title, _refresh_hp_layered (SAOPlayerGUI)
  * _run_exit_animation (SAOPlayerGUI)
"""

from __future__ import annotations

import os
import subprocess
import sys
import tkinter as tk
from typing import Any, Optional

from config import APP_VERSION_LABEL
from utils.sao_sound import get_cjk_font
from sao_theme import SAODialog


class SAOPlayerGUIDialogsMixin:
    """Mixin bundling player-panel factory + dialogs + small UI helpers."""

    def _hp_overlay_on_menu(self, x_root: int, y_root: int):
        """Right-click on HP panel: web-parity context menu."""
        try:
            menu = tk.Menu(self.root, tearoff=0,
                           bg='#cfd0c5', fg='#3c3e32',
                           activebackground='#e9ddb7',
                           activeforeground='#aa7814',
                           relief='flat', bd=1,
                           activeborderwidth=0,
                           font=get_cjk_font(9))
            menu.add_command(
                label='◆ SAO 菜单',
                command=self._hp_overlay_on_click,
            )
            recog_on = getattr(self, '_recognition_active', False)
            recog_label = '识别: ON' if recog_on else '识别: OFF'
            menu.add_command(
                label=f'◈ {recog_label}',
                command=self._toggle_recognition_menu,
            )
            menu.add_separator()
            menu.add_command(
                label='⟲ 复原位置',
                command=self._hp_overlay_restore_position,
            )
            menu.add_command(
                label='◈ 隐藏 HP 面板',
                command=self._hp_overlay_hide,
            )
            menu.add_separator()
            menu.add_command(
                label='✕ 退出',
                command=self._on_close,
            )
            try:
                menu.tk_popup(x_root, max(0, y_root - 90))
            finally:
                menu.grab_release()
        except Exception as exc:
            print(f'[SAO] HP overlay context menu failed: {exc}')

    def _show_welcome_then_menu(self):
        """首次启动: 平台只打开菜单；游戏资料由插件注入。"""
        self.root.after(300, self._toggle_sao_menu)

    def _show_entity_alert(self, title: str, message: str = '', display_time: float = 5.0):
        overlay = getattr(self, '_alert_overlay', None)
        if overlay is not None:
            try:
                overlay.show_alert(title, message, display_time=display_time)
                return
            except Exception:
                pass
        if message:
            print(f'[SAO Entity] {title}: {message}')
        else:
            print(f'[SAO Entity] {title}')

    def _spawn_webview_process(self) -> None:
        """Persist WebView mode, then launch a fresh main.py process."""
        try:
            settings_ref = getattr(self, '_cfg_settings_ref', None)
            if settings_ref is not None:
                settings_ref.set('ui_mode', 'webview')
                settings_ref.save()
                if getattr(self, 'settings', None) is not None and self.settings is not settings_ref:
                    try:
                        self.settings.set('ui_mode', 'webview')
                    except Exception:
                        pass
            elif getattr(self, 'settings', None) is not None:
                self.settings.set('ui_mode', 'webview')
                self.settings.save()
        except Exception as e:
            print(f'[SAO] Save WebView ui_mode failed: {e}')
        try:
            persist_identity = getattr(self, '_persist_cached_identity_state', None)
            if callable(persist_identity):
                persist_identity(save_now=True)
        except Exception as e:
            print(f'[SAO] Persist identity before WebView switch failed: {e}')
        try:
            if getattr(sys, 'frozen', False):
                args = [sys.executable]
                cwd = os.path.dirname(os.path.abspath(sys.executable))
            else:
                app_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
                args = [sys.executable, os.path.join(app_dir, 'main.py')]
                cwd = app_dir
            creationflags = 0
            if os.name == 'nt':
                creationflags = getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
            subprocess.Popen(
                args,
                cwd=cwd or None,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                close_fds=True,
                creationflags=creationflags,
            )
        except Exception as e:
            print(f"[SAO] Spawn WebView process failed: {e}")
            import traceback; traceback.print_exc()

    def _switch_to_webview_ui(self):
        """切换到 WebView UI — fresh process restart."""
        def _do_switch():
            self._run_exit_animation(
                after_shutdown=self._spawn_webview_process,
                mode='switch',
                target_label='SAO WEBVIEW UI',
                hard_exit=True,
            )

        SAODialog.ask(self._float, "切换 UI",
                      "将切换到 SAO WebView UI。\n确定继续吗？",
                      on_ok=_do_switch)

    def _show_about(self):
        if self._sao_menu is not None and self._sao_menu.visible:
            self._sao_menu.close()
        try:
            from updater.sao_updater import get_manager, STATE_AVAILABLE, STATE_READY
            st = get_manager().snapshot()
            extra = ''
            if st.state in (STATE_AVAILABLE, STATE_READY) and st.latest_version:
                tag = '已下载, 待重启' if st.state == STATE_READY else '可更新'
                extra = f"\n\n[{tag}] 新版本 v{st.latest_version}"
        except Exception:
            extra = ''
        self.root.after(600, lambda: SAODialog.showinfo(
            self._float, "关于",
            f"SAO Auto — Platform UI\n{APP_VERSION_LABEL}{extra}\n\n"
            "Alt+A 打开 SAO 菜单\n"
            "右键悬浮按钮查看更多选项"))

    def _show_license_panel_from_menu(self):
        if self._sao_menu is not None and self._sao_menu.visible:
            self._sao_menu.close()
        from gui_modules.sao_gui_license import reset_license_dialog_dismissed, show_license_dialog
        reset_license_dialog_dismissed()
        self.root.after(400, lambda: show_license_dialog(self._float))

    def _edit_profile(self):
        """Game profile editing is plugin-owned."""
        handler = getattr(self, '_plugin_edit_profile', None)
        if callable(handler):
            return handler()
        return None
