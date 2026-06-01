# -*- coding: utf-8 -*-
"""
SAOPlayerGUIDialogsMixin — eleventh mixin extracted from
SAOPlayerGUI (round 52 of the sao_gui split refactor). 7 methods,
~197 lines.

A small grab-bag mixin for SAOPlayerGUI helpers that build modal
dialogs, the SAO menu's left widget, and the right-click HP
overlay context menu. Common theme: each one is a UI-construction
helper that has no natural home in the earlier domain-specific
mixins.

Methods:
  * _make_player_panel(parent) — factory called by the SAO menu's
    SAOPopUpMenu as ``left_widget_factory``. Constructs the
    SAOMenuLeftStack (player info + session-players panel) and
    wires its mode-change callback back to settings persistence.
  * _hp_overlay_on_menu(x_root, y_root) — right-click context menu
    for the floating HP panel: SAO Menu / recognition toggle /
    restore-position / hide-panel / exit.
  * _show_welcome_then_menu — first-launch flow: profile dialog
    first, SAO menu second.
  * _show_entity_alert(title, message='', display_time=5.0) —
    convenience around self._alert_overlay.show_alert with a
    print() fallback.
  * _switch_to_webview_ui — confirm dialog + exit-animation + hot
    restart into sao_webview.SAOWebViewGUI.
  * _show_about — SAO 'about' info dialog with updater state hint.
  * _edit_profile — opens the profile editor (SAODialog-backed
    show_welcome_dialog) with anti-double-open guard +
    redraw-on-done plumbing.

Required SAOPlayerGUI attrs:
  * self._username, self._profession, self._level, self._level_extra,
    self._season_exp
  * self._menu_left_stack, self._player_panel,
    self._session_players_panel, self._sao_menu
  * self._sta_hp, self._sta_sta
  * self._alert_overlay, self._hp_overlay, self._recognition_active
  * self._float, self.root, self.settings, self._cfg_settings_ref
  * self._profile_dialog_ref, self._profile_dialog_pending
  * self._after_shutdown

Required SAOPlayerGUI methods (via MRO):
  * _get_session_player_rows, _refresh_session_players_panel
    (Session mixin)
  * _get_setting, _set_setting (SAOPlayerGUI)
  * _toggle_sao_menu (Menu mixin)
  * _toggle_recognition_menu (Panels mixin)
  * _hp_overlay_on_click, _hp_overlay_restore_position,
    _hp_overlay_hide, _on_close (SAOPlayerGUI)
  * _update_float_title, _refresh_hp_layered (SAOPlayerGUI)
  * _run_exit_animation (SAOPlayerGUI)
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Optional

from config import APP_VERSION_LABEL
from utils.sao_sound import get_cjk_font
from sao_theme import SAODialog
from engines.character_profile import show_welcome_dialog
from gui_modules.sao_menu_left_stack import SAOMenuLeftStack


class SAOPlayerGUIDialogsMixin:
    """Mixin bundling player-panel factory + dialogs + small UI helpers."""

    def _make_player_panel(self, parent):
        """工厂: 为 SAO 菜单创建左侧信息面板"""
        stack = SAOMenuLeftStack(
            parent,
            username=self._username or 'Player',
            profession=self._profession or '',
            rows_provider=self._get_session_player_rows,
        )
        panel = stack.player_panel
        self._menu_left_stack = stack
        self._player_panel = panel
        self._session_players_panel = stack.session_panel
        try:
            stack.session_panel.bind_global_wheel_fallback()
        except Exception:
            pass

        panel.update_level(self._level, self._level_extra, self._season_exp)

        # HP / STA 数据 (来自识别引擎)
        panel._sta_hp = getattr(self, '_sta_hp', (0, 0))
        panel._sta_sta = getattr(self, '_sta_sta', (0, 0))

        # 菜单模式 (从 settings 恢复)
        saved_mode = self._get_setting('shift_mode', '普通模式')
        if saved_mode:
            panel._shift_mode = saved_mode

        # 模式变更 → 自动保存
        panel._on_mode_change = lambda m: self._set_setting('shift_mode', m)

        self._refresh_session_players_panel(force=True)
        return stack

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
        except Exception:
            pass

    def _show_welcome_then_menu(self):
        """首次启动: 显示欢迎对话框, 完成后再打开菜单"""
        def on_profile_done(username, profession):
            self._username = username
            self._profession = profession
            self._update_float_title()
            # 更新 SAO 菜单的用户信息
            if self._sao_menu:
                self._sao_menu.username = username
                self._sao_menu.description = profession or 'SAO Auto — 游戏辅助 UI'
            self.root.after(300, self._toggle_sao_menu)

        show_welcome_dialog(self._float, on_done=on_profile_done)

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

    def _switch_to_webview_ui(self):
        """切换到 WebView UI (sao_webview.py) — 热切换"""
        def _do_switch():
            def _launch_next():
                import gc; gc.collect()
                time.sleep(0.3)
                try:
                    from sao_webview import SAOWebViewGUI
                    app = SAOWebViewGUI()
                    app.run()
                except Exception as e:
                    print(f"[SAO] Hot switch to WebView failed: {e}")
                    import traceback; traceback.print_exc()

            self._after_shutdown = _launch_next
            self._run_exit_animation(after_shutdown=None,
                                     mode='switch', target_label='SAO WEBVIEW UI')

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
            f"SAO Auto — 游戏辅助 UI\n{APP_VERSION_LABEL}{extra}\n\n"
            "Alt+A 打开 SAO 菜单\n"
            "右键悬浮按钮查看更多选项"))

    def _edit_profile(self):
        """打开角色资料编辑对话框"""
        dialog = getattr(self, '_profile_dialog_ref', None)
        try:
            dlg_win = getattr(dialog, '_dlg', None)
            if dlg_win is not None and dlg_win.winfo_exists():
                dlg_win.lift()
                dlg_win.focus_force()
                return
        except Exception:
            pass
        if getattr(self, '_profile_dialog_pending', False):
            return
        self._profile_dialog_pending = True
        if self._sao_menu is not None and self._sao_menu.visible:
            self._sao_menu.close()

        def on_profile_done(username, profession):
            self._profile_dialog_pending = False
            self._profile_dialog_ref = None
            self._username = username
            self._profession = profession
            self._update_float_title()
            if self._sao_menu:
                self._sao_menu.username = username
                self._sao_menu.description = profession or 'SAO Auto — 游戏辅助 UI'
            if self._player_panel:
                self._player_panel._username = username
                self._player_panel._profession = profession
                if self._player_panel._active:
                    self._player_panel._redraw_top(
                        self._player_panel._target_w,
                        self._player_panel._top_h)

        def _open_profile_dialog():
            self._profile_dialog_pending = False
            try:
                dialog = show_welcome_dialog(self._float, on_done=on_profile_done)
                self._profile_dialog_ref = dialog
                dlg_win = getattr(dialog, '_dlg', None)
                if dlg_win is not None:
                    def _clear_ref(_event=None, ref=dialog):
                        if getattr(self, '_profile_dialog_ref', None) is ref:
                            self._profile_dialog_ref = None
                        self._profile_dialog_pending = False
                    try:
                        dlg_win.bind('<Destroy>', _clear_ref, add='+')
                    except Exception:
                        pass
            except Exception:
                self._profile_dialog_ref = None
                self._profile_dialog_pending = False

        self.root.after(600, _open_profile_dialog)

