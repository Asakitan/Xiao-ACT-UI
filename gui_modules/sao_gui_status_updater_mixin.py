# -*- coding: utf-8 -*-
"""
SAOPlayerGUIStatusUpdaterMixin — tenth mixin extracted from
SAOPlayerGUI (round 50 of the sao_gui split refactor). 20 methods,
~775 lines.

Bundles the two floating panels that are tightly coupled around
the auto-updater event chain:

Status panel (2 methods) — small panel showing recognition state +
data source + updater status:
  * _toggle_status_panel — open/close; builds the panel UI when
    opening (uses the panel-UI helpers from gui_modules.sao_panel_ui)
  * _update_status_panel — refresh the panel's content rows from
    current state + updater snapshot

Updater event chain (18 methods):
  Snapshot + view:
    * _get_update_snapshot — sao_updater.get_manager().snapshot()
    * _get_update_view(snapshot=None) — formats the snapshot into
      status_text / progress_text / colors for the panel (139 lines —
      the biggest method here; lots of state-text formatting)
  Listener wiring:
    * _ensure_updater_listener — installs the snapshot listener;
      lazy-init on first use
    * _on_update_snapshot — listener body, dispatches refresh +
      popup logic on the Tk main thread via root.after(0, ...)
  Update popup:
    * _mark_update_popup_ready — flips the ready flag; flushes
      pending snapshot
    * _build_update_popup_payload — builds the SAODialog payload
    * _maybe_show_update_popup — gates the popup behind state +
      ready + force-required logic
  Action helpers:
    * _start_update_download, _start_update_check,
      _skip_update_version, _apply_downloaded_update
    * _resolve_update_action(action_name) — string → callable
    * _set_update_button — install a labelled button on the panel
  Update panel:
    * _open_update_panel — builds the panel UI (111 lines)
    * _refresh_update_panel — re-populate based on snapshot
    * _close_update_panel — fade out + persist hidden flag
  Menu hooks:
    * _check_for_updates_interactive — menu handler
    * _prompt_update_available — show the SAODialog promo

Required SAOPlayerGUI attrs (extensive):
  * self._status_panel, self._status_recog_lbl, self._status_source_lbl,
    self._status_update_lbl, self._status_update_progress_lbl
  * self._update_panel, self._update_snapshot,
    self._update_panel_hidden, self._update_panel_state_key,
    self._update_listener_installed, self._update_listener,
    self._updater_mgr, self._update_popup_ready,
    self._pending_update_popup_snapshot
  * self._float, self._fw, self._panels_hidden, self.root, self.settings,
    self._cfg_settings_ref

Required SAOPlayerGUI methods (via MRO):
  * _fade_panel_in, _fade_panel_out, _raise_panel_window,
    _attach_sao_panel_fx, _attach_panel_float (SAOPlayerGUI)
  * _refresh_menu_if_open (Menu mixin)
  * _show_entity_alert, _get_setting, _set_setting (SAOPlayerGUI)
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, Callable, Dict, Optional

from config import APP_VERSION_LABEL
from utils.sao_sound import play_sound, get_sao_font, get_cjk_font
from sao_theme import (
    SAOButton, SAOProgressBar, SAOStatusPill, SAODialog,
)
from gui_modules.sao_panel_ui import (
    _apply_panel_style, _sao_panel_header, _bind_panel_drag,
    _sao_panel_body, _sao_panel_hud_canvas, _sao_row, _sao_pill,
    _SAO_PANEL_HEADER_BG, _SAO_PANEL_BORDER, _SAO_PANEL_BODY_BG,
    _SAO_PANEL_ACCENT, _SAO_PANEL_GOLD, _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
)


class SAOPlayerGUIStatusUpdaterMixin:
    """Mixin bundling status panel + updater event chain + update panel."""

    def _toggle_status_panel(self):
        """浮动状态面板 — 显示识别状态 + 引擎信息"""
        if self._status_panel and self._status_panel.winfo_exists():
            self._fade_panel_out(self._status_panel, '_status_panel', 'show_status')
            return

        try: play_sound('panel')
        except: pass
        sw, sh = 236, 178
        saved_sx = self.settings.get('status_x', None)
        saved_sy = self.settings.get('status_y', None)
        if saved_sx is not None:
            fx, fy = int(saved_sx), int(saved_sy)
        else:
            fx = self._float.winfo_x() + self._fw + 10
            fy = self._float.winfo_y()
            if fx + sw > self._float.winfo_screenwidth() - 10:
                fx = self._float.winfo_x() - sw - 10

        self._status_panel = tk.Toplevel(self.root)
        self._status_panel.overrideredirect(True)
        self._status_panel.attributes('-topmost', True)
        self._status_panel.attributes('-alpha', 0.0)
        self._status_panel.geometry(f'{sw}x{sh}+{fx}+{fy}')
        self._status_panel.configure(bg=_SAO_PANEL_HEADER_BG)
        _apply_panel_style(self._status_panel)

        border = tk.Frame(self._status_panel, bg=_SAO_PANEL_BORDER, padx=1, pady=1)
        border.pack(fill=tk.BOTH, expand=True)
        inner = tk.Frame(border, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill=tk.BOTH, expand=True)

        hdr, close_lbl = _sao_panel_header(inner, '◉', 'STATUS', self._toggle_status_panel)

        body = _sao_panel_body(inner)
        body_pad = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        body_pad.pack(fill=tk.BOTH, expand=True, padx=10, pady=6)

        # 识别状态行
        recog_text = 'ON' if getattr(self, '_recognition_active', False) else 'OFF'
        recog_fg = '#3ad86c' if recog_text == 'ON' else '#556677'
        self._status_recog_lbl = _sao_row(body_pad, '识别', recog_text,
                                           value_fg=recog_fg,
                                           value_font=get_cjk_font(9, True))

        # 数据源行
        src_text = 'Packet'
        if getattr(self, '_cfg_settings_ref', None):
            src = str(self._cfg_settings_ref.get('mem_data_source', 'tcp') or 'tcp').lower()
            src_text = {'tcp': 'TCP', 'memory': 'MEM', 'hybrid': 'HYBRID', 'auto': 'AUTO'}.get(src, 'TCP')
        self._status_source_lbl = _sao_row(body_pad, '数据源', src_text,
                                            value_fg=_SAO_PANEL_GOLD)

        # 更新状态行
        self._status_update_lbl = _sao_row(body_pad, '更新', '待机',
                                           value_fg='#556677',
                                           value_font=get_cjk_font(9, True))
        self._status_update_progress_lbl = _sao_row(body_pad, '进度', '--',
                                                    value_fg=_SAO_PANEL_ACCENT,
                                                    value_font=get_cjk_font(9, True))

        # 底部 HUD 装饰
        hud_cv = _sao_panel_hud_canvas(body)
        hud_cv.create_text(4, 8, text='SYS:STATUS', anchor='w',
                           font=('Consolas', 6), fill='#d0d0d0')
        hud_cv.create_line(80, 8, sw - 10, 8, fill='#e8e8e8', width=1)

        # 拖拽
        _sd = {'x': 0, 'y': 0}
        def sdstart(e): _sd['x'], _sd['y'] = e.x_root, e.y_root
        def sdmove(e):
            dx, dy = e.x_root - _sd['x'], e.y_root - _sd['y']
            nx, ny = self._status_panel.winfo_x()+dx, self._status_panel.winfo_y()+dy
            self._status_panel.geometry(f'+{nx}+{ny}')
            _sd['x'], _sd['y'] = e.x_root, e.y_root
            self.settings.set('status_x', nx); self.settings.set('status_y', ny)
        _bind_panel_drag(hdr, close_lbl, sdstart, sdmove)

        self._fade_panel_in(self._status_panel, target=0.92)
        self._attach_sao_panel_fx(self._status_panel, hdr, inner)
        self._attach_panel_float(self._status_panel, phase=2.0)
        self._update_status_panel()
        self.settings.set('show_status', True)
        self.settings.save()

    def _update_status_panel(self):
        """刷新状态面板内容"""
        if not (self._status_panel and self._status_panel.winfo_exists()):
            return
        # 识别状态
        if hasattr(self, '_status_recog_lbl'):
            recog_text = 'ON' if getattr(self, '_recognition_active', False) else 'OFF'
            recog_fg = '#3ad86c' if recog_text == 'ON' else '#556677'
            self._status_recog_lbl.configure(text=recog_text, fg=recog_fg)
        if hasattr(self, '_status_source_lbl'):
            src_text = 'Packet'
            if getattr(self, '_cfg_settings_ref', None):
                src = str(self._cfg_settings_ref.get('mem_data_source', 'tcp') or 'tcp').lower()
                src_text = {'tcp': 'TCP', 'memory': 'MEM', 'hybrid': 'HYBRID', 'auto': 'AUTO'}.get(src, 'TCP')
            self._status_source_lbl.configure(text=src_text, fg=_SAO_PANEL_GOLD)
        view = self._get_update_view()
        if hasattr(self, '_status_update_lbl'):
            self._status_update_lbl.configure(text=view['status_text'], fg=view['status_color'])
        if hasattr(self, '_status_update_progress_lbl'):
            self._status_update_progress_lbl.configure(text=view['progress_text'], fg=view['progress_color'])

    def _get_update_snapshot(self):
        try:
            from updater.sao_updater import get_manager
            return get_manager().snapshot()
        except Exception:
            return None

    def _get_update_view(self, snapshot=None):
        snap = snapshot or self._update_snapshot or self._get_update_snapshot()
        state = getattr(snap, 'state', 'idle') if snap else 'idle'
        latest_version = str(getattr(snap, 'latest_version', '') or '') if snap else ''
        package_type = str(getattr(snap, 'package_type', '') or '') if snap else ''
        notes = str(getattr(snap, 'notes', '') or '').strip() if snap else ''
        error = str(getattr(snap, 'error', '') or '').strip() if snap else ''
        skipped_version = str(getattr(snap, 'skipped_version', '') or '') if snap else ''
        force_required = bool(getattr(snap, 'force_required', False)) if snap else False
        try:
            progress = max(0.0, min(1.0, float(getattr(snap, 'progress', 0.0) or 0.0))) if snap else 0.0
        except Exception:
            progress = 0.0
        package_text = '模块增量包' if package_type == 'runtime-delta' else ('完整更新包' if package_type == 'full-package' else '更新包')
        view = {
            'state': state,
            'latest_version': latest_version,
            'force_required': force_required,
            'progress': progress,
            'show_panel': False,
            'show_progress': False,
            'status_text': '待机',
            'status_color': '#556677',
            'progress_text': '--',
            'progress_color': '#8896a8',
            'badge_text': 'IDLE',
            'badge_color': '#556677',
            'headline': '等待更新检查',
            'version_text': f'当前 {APP_VERSION_LABEL}',
            'meta_text': '菜单中的“检查更新”可主动拉取远端版本信息。',
            'notes_text': '当检测到新版本时，将自动显示下载与重启入口。',
            'primary_text': '检查更新',
            'primary_action': 'check',
            'secondary_text': '',
            'secondary_action': None,
        }
        if state == 'checking':
            view.update({
                'status_text': '检查中',
                'status_color': _SAO_PANEL_ACCENT,
                'badge_text': 'CHECKING',
                'badge_color': _SAO_PANEL_ACCENT,
                'headline': '正在检查更新',
                'meta_text': '正在连接更新服务…',
                'notes_text': '请稍候，系统正在比较本地版本与远端 manifest。',
                'primary_text': '检查中...',
                'primary_action': None,
            })
        elif state == 'available':
            skipped = bool(latest_version and skipped_version == latest_version and not force_required)
            view.update({
                'show_panel': not skipped,
                'status_text': '已跳过' if skipped else '可更新',
                'status_color': '#8896a8' if skipped else _SAO_PANEL_GOLD,
                'progress_text': latest_version and f'v{latest_version}' or '--',
                'progress_color': '#8896a8' if skipped else _SAO_PANEL_GOLD,
                'badge_text': 'SKIPPED' if skipped else ('REQUIRED' if force_required else 'AVAILABLE'),
                'badge_color': '#8896a8' if skipped else _SAO_PANEL_GOLD,
                'headline': '已跳过当前版本' if skipped else '检测到新版本',
                'version_text': latest_version and f'新版本 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': ('强制更新 · ' if force_required else '') + package_text,
                'notes_text': notes or ('当前版本低于服务器最低要求，请先完成更新。' if force_required else ('该版本已被标记为跳过，可手动重新检查或等待下一版。' if skipped else '下载完成后，重启应用将自动进入独立更新器。')),
                'primary_text': '重新检查' if skipped else '立即更新',
                'primary_action': 'check' if skipped else 'download',
                'secondary_text': '' if skipped else ('退出应用' if force_required else '跳过此版'),
                'secondary_action': None if skipped else ('quit' if force_required else 'skip'),
            })
        elif state == 'downloading':
            view.update({
                'show_panel': True,
                'show_progress': True,
                'status_text': f'下载中 {int(progress * 100)}%',
                'status_color': _SAO_PANEL_ACCENT,
                'progress_text': f'{int(progress * 100)}%',
                'progress_color': _SAO_PANEL_ACCENT,
                'badge_text': 'DOWNLOADING',
                'badge_color': _SAO_PANEL_ACCENT,
                'headline': '正在下载更新包',
                'version_text': latest_version and f'目标 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': f'正在拉取 {package_text}',
                'notes_text': notes or '下载完成后可直接重启应用，文件替换会交给独立 updater 完成。',
                'primary_text': '下载中...',
                'primary_action': None,
                'secondary_text': '',
                'secondary_action': None,
            })
        elif state == 'ready':
            view.update({
                'show_panel': True,
                'show_progress': True,
                'progress': 1.0,
                'status_text': '已就绪',
                'status_color': '#3ad86c',
                'progress_text': '100%',
                'progress_color': '#3ad86c',
                'badge_text': 'READY',
                'badge_color': '#3ad86c',
                'headline': '更新包已下载完成',
                'version_text': latest_version and f'待切换 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': '重启应用后将进入独立更新器完成替换',
                'notes_text': notes or '当前下载已完成，点击“重启应用”即可开始替换模块与启动器。',
                'primary_text': '重启应用',
                'primary_action': 'apply',
                'secondary_text': '' if force_required else '稍后',
                'secondary_action': None if force_required else 'dismiss',
            })
        elif state == 'error':
            view.update({
                'show_panel': True,
                'status_text': '错误',
                'status_color': '#ff707a',
                'progress_text': '失败',
                'progress_color': '#ff707a',
                'badge_text': 'ERROR',
                'badge_color': '#ff707a',
                'headline': '更新流程失败',
                'version_text': latest_version and f'目标 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': error or '更新服务暂不可用',
                'notes_text': error or '可以稍后重新检查一次。',
                'primary_text': '重新检查',
                'primary_action': 'check',
                'secondary_text': '关闭',
                'secondary_action': 'dismiss',
            })
        elif state == 'up_to_date':
            view.update({
                'status_text': '最新',
                'status_color': _SAO_PANEL_ACCENT,
                'progress_text': latest_version and f'v{latest_version}' or APP_VERSION_LABEL,
                'progress_color': _SAO_PANEL_ACCENT,
                'badge_text': 'LATEST',
                'badge_color': _SAO_PANEL_ACCENT,
                'headline': '当前已是最新版本',
                'version_text': latest_version and f'当前 v{latest_version}' or f'当前 {APP_VERSION_LABEL}',
                'meta_text': '无需下载更新包',
                'notes_text': '未检测到比当前客户端更高的版本。',
            })
        return view

    def _ensure_updater_listener(self):
        if getattr(self, '_update_listener_installed', False):
            return
        try:
            from updater.sao_updater import get_manager
            mgr = get_manager()
        except Exception:
            return

        def _listener(snapshot):
            try:
                self.root.after(0, lambda snap=snapshot: self._on_update_snapshot(snap))
            except Exception:
                pass

        self._update_listener = _listener
        self._updater_mgr = mgr
        mgr.add_listener(_listener)
        self._update_listener_installed = True
        try:
            self._on_update_snapshot(mgr.snapshot())
        except Exception:
            pass

    def _on_update_snapshot(self, snapshot):
        self._update_snapshot = snapshot
        view = self._get_update_view(snapshot)
        panel_key = f"{view['state']}:{view['latest_version']}"
        if panel_key != getattr(self, '_update_panel_state_key', ''):
            self._update_panel_hidden = False
        prev_panel_key = getattr(self, '_update_panel_state_key', '')
        self._update_panel_state_key = panel_key

        self._update_status_panel()
        # v2.1.2-l: 仅在状态/版本切换时刷菜单 (避免下载进度变化每秒触发
        # 整菜单 rebuild → entity HUD 撕裂). 状态面板的进度行依旧每帧实时.
        if panel_key != prev_panel_key:
            self._refresh_menu_if_open()

        if self._update_panel and self._update_panel.winfo_exists():
            if view['show_panel']:
                self._refresh_update_panel(snapshot)
            else:
                self._close_update_panel(persist_hidden=False)

        if not getattr(self, '_update_popup_ready', False):
            self._pending_update_popup_snapshot = snapshot
            return

        self._maybe_show_update_popup(snapshot)

        if self._panels_hidden:
            if not view['show_panel']:
                try:
                    if self._update_panel and self._update_panel.winfo_exists():
                        self._update_panel.destroy()
                except Exception:
                    pass
                self._update_panel = None
                try:
                    self._hidden_panels_snapshot = [name for name in self._hidden_panels_snapshot if name != 'updater']
                except Exception:
                    pass
            return

    def _mark_update_popup_ready(self):
        self._update_popup_ready = True
        pending = getattr(self, '_pending_update_popup_snapshot', None)
        if pending is None:
            return
        self._pending_update_popup_snapshot = None
        self._maybe_show_update_popup(pending)

    def _build_update_popup_payload(self, snapshot=None):
        view = self._get_update_view(snapshot)
        state = str(view.get('state') or 'idle')
        latest_version = str(view.get('latest_version') or '')
        force_required = bool(view.get('force_required', False))
        progress = int(round(float(view.get('progress') or 0.0) * 100.0))
        skipped_version = ''
        try:
            skipped_version = str(getattr(snapshot, 'skipped_version', '') or '')
        except Exception:
            skipped_version = ''
        if state == 'available':
            if latest_version and skipped_version == latest_version and not force_required:
                return None
            body = f'检测到新版本 v{latest_version or "?"}'
            if force_required:
                body += '\n此更新为强制更新，请尽快在 关于 > 检查更新 中完成下载。'
            else:
                body += '\n打开 SAO 菜单 > 关于 > 检查更新 可开始下载。'
            return {
                'key': f'available:{latest_version}:{int(force_required)}',
                'title': 'SYSTEM UPDATE',
                'message': body,
                'display_time': 6.5,
            }
        if state == 'downloading':
            body = f'正在下载更新包 v{latest_version or "?"}'
            body += f'\n当前进度 {progress}% ，完成后会提示重启应用。'
            return {
                'key': f'downloading:{latest_version}',
                'title': 'DOWNLOADING UPDATE',
                'message': body,
                'display_time': 5.0,
            }
        if state == 'ready':
            body = f'更新包 v{latest_version or "?"} 已下载完成'
            body += '\n打开 SAO 菜单 > 关于 > 检查更新 可立即重启应用。'
            return {
                'key': f'ready:{latest_version}',
                'title': 'UPDATE READY',
                'message': body,
                'display_time': 6.5,
            }
        if state == 'error':
            # Startup checks can fail transiently while DNS/network is still
            # warming up. Keep those errors in the updater panel and in the
            # manual-check dialog, but do not surface a scary entity alert.
            return None
        return None

    def _maybe_show_update_popup(self, snapshot=None):
        # v2.1.2-l: 防止"撞屋"式多弹窗:
        #   1) 如果专用更新面板 (_update_panel) 已经可见, 不再弹 entity alert
        #      — 信息已经更显眼地呈现在面板里, 重复弹只会刷屏。
        #   2) downloading 状态本身就有进度条 (面板/状态面板) 在持续显示,
        #      不需要再来一发短暂的 entity alert。
        #   只保留 available / ready 这种"重要事件型"的一次性提示。
        try:
            panel_visible = bool(self._update_panel and self._update_panel.winfo_exists())
        except Exception:
            panel_visible = False
        payload = self._build_update_popup_payload(snapshot)
        if not payload:
            return
        popup_key = str(payload.get('key') or '')
        if not popup_key or popup_key == getattr(self, '_last_update_popup_key', ''):
            return
        if popup_key.startswith('downloading:'):
            # 下载进度由专用面板/状态面板显示, 不再 entity 弹窗。
            self._last_update_popup_key = popup_key
            return
        if panel_visible:
            # 已经有面板在前台讲同一件事, 静音 entity alert。
            self._last_update_popup_key = popup_key
            return
        self._last_update_popup_key = popup_key
        self._show_entity_alert(
            str(payload.get('title') or 'SYSTEM UPDATE'),
            str(payload.get('message') or ''),
            display_time=float(payload.get('display_time') or 5.0),
        )

    def _start_update_download(self):
        try:
            from updater.sao_updater import get_manager
            get_manager().download_async()
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'下载启动失败: {e}')

    def _start_update_check(self):
        try:
            from updater.sao_updater import get_manager
            get_manager().check_async()
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'检查失败: {e}')

    def _skip_update_version(self):
        try:
            from updater.sao_updater import get_manager
            get_manager().skip_current()
            self._close_update_panel(persist_hidden=True)
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'跳过版本失败: {e}')

    def _apply_downloaded_update(self):
        try:
            from updater.sao_updater import has_pending_update, schedule_apply_on_exit
            if not has_pending_update():
                SAODialog.showinfo(self._float, '更新', '当前没有待应用的更新包。')
                return
            if not schedule_apply_on_exit():
                SAODialog.showinfo(self._float, '更新', '启动 updater 失败，请稍后重试。')
                return
            self._show_entity_alert('SYSTEM UPDATE', '正在切换到独立更新器', display_time=2.4)
            self._on_close()
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'应用更新失败: {e}')

    def _resolve_update_action(self, action_name):
        return {
            'download': self._start_update_download,
            'check': self._start_update_check,
            'skip': self._skip_update_version,
            'apply': self._apply_downloaded_update,
            'dismiss': lambda: self._close_update_panel(persist_hidden=True),
            'quit': self._on_close,
        }.get(action_name)

    def _set_update_button(self, button, text, action_name, side=tk.LEFT):
        if not text:
            if button.winfo_manager():
                button.pack_forget()
            button.command = None
            return
        button.set_text(text)
        button.command = self._resolve_update_action(action_name)
        if not button.winfo_manager():
            button.pack(side=side, padx=4)

    def _close_update_panel(self, persist_hidden=True):
        if persist_hidden:
            self._update_panel_hidden = True
        else:
            self._update_panel_hidden = False
        panel = self._update_panel
        self._update_panel = None
        if not (panel and panel.winfo_exists()):
            return
        try: play_sound('alert_close')
        except: pass
        t0 = time.time()
        dur = 0.22

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            t = min(1.0, (time.time() - t0) / dur)
            et = t * t
            try:
                panel.attributes('-alpha', max(0.0, 0.92 * (1.0 - et)))
            except Exception:
                pass
            if t < 1.0:
                try:
                    self.root.after(16, _step)
                except Exception:
                    pass
            else:
                try:
                    panel.destroy()
                except Exception:
                    pass

        _step()

    def _open_update_panel(self, snapshot=None):
        if self._panels_hidden:
            return
        self._update_panel_hidden = False
        if self._update_panel and self._update_panel.winfo_exists():
            try:
                self._update_panel.deiconify()
                self._update_panel.lift()
            except Exception:
                pass
            self._refresh_update_panel(snapshot)
            return

        try: play_sound('panel')
        except: pass
        sw, sh = 320, 228
        saved_ux = self.settings.get('update_x', None)
        saved_uy = self.settings.get('update_y', None)
        if saved_ux is not None:
            fx, fy = int(saved_ux), int(saved_uy)
        else:
            fx = self._float.winfo_x() + self._fw + 10
            fy = self._float.winfo_y() + 110
            screen_w = self._float.winfo_screenwidth()
            screen_h = self._float.winfo_screenheight()
            if fx + sw > screen_w - 10:
                fx = self._float.winfo_x() - sw - 10
            if fy + sh > screen_h - 10:
                fy = max(20, self._float.winfo_y() - sh + 36)

        self._update_panel = tk.Toplevel(self.root)
        self._update_panel.overrideredirect(True)
        self._update_panel.attributes('-topmost', True)
        self._update_panel.attributes('-alpha', 0.0)
        self._update_panel.geometry(f'{sw}x{sh}+{fx}+{fy}')
        self._update_panel.configure(bg=_SAO_PANEL_HEADER_BG)
        _apply_panel_style(self._update_panel)

        border = tk.Frame(self._update_panel, bg=_SAO_PANEL_BORDER, padx=1, pady=1)
        border.pack(fill=tk.BOTH, expand=True)
        inner = tk.Frame(border, bg=_SAO_PANEL_BODY_BG)
        inner.pack(fill=tk.BOTH, expand=True)

        hdr, close_lbl = _sao_panel_header(inner, '⬇', 'UPDATER', lambda: self._close_update_panel(True))
        body = _sao_panel_body(inner)
        body_pad = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        body_pad.pack(fill=tk.BOTH, expand=True, padx=10, pady=8)

        top_row = tk.Frame(body_pad, bg=_SAO_PANEL_BODY_BG)
        top_row.pack(fill=tk.X)
        self._update_badge = SAOStatusPill(top_row, text='IDLE', color='#556677', width=126, height=22)
        self._update_badge.pack(side=tk.LEFT)
        self._update_version_lbl = tk.Label(top_row, text=f'当前 {APP_VERSION_LABEL}',
                                            bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_GOLD,
                                            font=get_cjk_font(8, True), anchor='e')
        self._update_version_lbl.pack(side=tk.RIGHT)

        self._update_headline_lbl = tk.Label(body_pad, text='等待更新检查',
                                             bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                                             anchor='w', justify=tk.LEFT,
                                             font=get_cjk_font(10, True))
        self._update_headline_lbl.pack(fill=tk.X, pady=(8, 2))
        # meta_text 包含中文 (例如 “正在拉取 ...”), 必须用 CJK 字体,
        # 否则 SAO UI 字体没有对应字形, 整行会显示成方框 tofu。
        self._update_meta_lbl = tk.Label(body_pad, text='',
                                         bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_LABEL_FG,
                                         anchor='w', justify=tk.LEFT,
                                         font=get_cjk_font(8))
        self._update_meta_lbl.pack(fill=tk.X)
        self._update_notes_lbl = tk.Label(body_pad, text='',
                                          bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_VALUE_FG,
                                          anchor='w', justify=tk.LEFT,
                                          wraplength=286,
                                          font=get_cjk_font(8))
        self._update_notes_lbl.pack(fill=tk.X, pady=(6, 8))

        self._update_progress_wrap = tk.Frame(body_pad, bg=_SAO_PANEL_BODY_BG)
        self._update_progress_wrap.pack(fill=tk.X)
        self._update_progress_bar = SAOProgressBar(self._update_progress_wrap, width=286, height=18)
        self._update_progress_bar.pack(fill=tk.X)
        self._update_progress_lbl = tk.Label(self._update_progress_wrap, text='0%',
                                             bg=_SAO_PANEL_BODY_BG, fg=_SAO_PANEL_ACCENT,
                                             anchor='e', justify=tk.RIGHT,
                                             font=get_sao_font(7, True))
        self._update_progress_lbl.pack(fill=tk.X, pady=(3, 0))

        self._update_buttons_frame = tk.Frame(body_pad, bg=_SAO_PANEL_BODY_BG)
        self._update_buttons_frame.pack(fill=tk.X, pady=(10, 0))
        self._update_primary_btn = SAOButton(self._update_buttons_frame, width=134, height=32)
        self._update_secondary_btn = SAOButton(self._update_buttons_frame, width=134, height=32)

        hud_cv = _sao_panel_hud_canvas(body)
        hud_cv.create_text(4, 8, text='SYS:UPDATER', anchor='w',
                           font=('Consolas', 6), fill='#d0d0d0')
        hud_cv.create_line(92, 8, sw - 10, 8, fill='#e8e8e8', width=1)

        _ud = {'x': 0, 'y': 0}
        def udstart(e): _ud['x'], _ud['y'] = e.x_root, e.y_root
        def udmove(e):
            dx, dy = e.x_root - _ud['x'], e.y_root - _ud['y']
            nx, ny = self._update_panel.winfo_x()+dx, self._update_panel.winfo_y()+dy
            self._update_panel.geometry(f'+{nx}+{ny}')
            _ud['x'], _ud['y'] = e.x_root, e.y_root
            self.settings.set('update_x', nx); self.settings.set('update_y', ny)
        _bind_panel_drag(hdr, close_lbl, udstart, udmove)

        self._fade_panel_in(self._update_panel, target=0.92)
        self._attach_sao_panel_fx(self._update_panel, hdr, inner)
        self._attach_panel_float(self._update_panel, phase=4.1)
        self._refresh_update_panel(snapshot)

    def _refresh_update_panel(self, snapshot=None):
        if not (self._update_panel and self._update_panel.winfo_exists()):
            return
        view = self._get_update_view(snapshot)
        if hasattr(self, '_update_badge'):
            self._update_badge.set_status(view['badge_text'], view['badge_color'])
        if hasattr(self, '_update_version_lbl'):
            self._update_version_lbl.configure(text=view['version_text'])
        if hasattr(self, '_update_headline_lbl'):
            self._update_headline_lbl.configure(text=view['headline'])
        if hasattr(self, '_update_meta_lbl'):
            self._update_meta_lbl.configure(text=view['meta_text'])
        if hasattr(self, '_update_notes_lbl'):
            self._update_notes_lbl.configure(text=view['notes_text'])
        if hasattr(self, '_update_progress_wrap'):
            if view['show_progress']:
                if not self._update_progress_wrap.winfo_manager():
                    self._update_progress_wrap.pack(fill=tk.X, before=self._update_buttons_frame)
                self._update_progress_bar.set_value(view['progress'])
                self._update_progress_lbl.configure(text=view['progress_text'], fg=view['progress_color'])
            elif self._update_progress_wrap.winfo_manager():
                self._update_progress_wrap.pack_forget()
        self._set_update_button(self._update_primary_btn, view['primary_text'], view['primary_action'], side=tk.LEFT)
        self._set_update_button(self._update_secondary_btn, view['secondary_text'], view['secondary_action'], side=tk.RIGHT)

    def _defer_update_check_until_menu_closed(self, callback: Callable[[], None]) -> bool:
        """Delay manual update UI until the GPU popup has finished closing."""
        if getattr(self, '_destroyed', False):
            return True
        menu = getattr(self, '_sao_menu', None)
        try:
            menu_visible = bool(menu is not None and getattr(menu, 'visible', False))
        except Exception:
            menu_visible = False
        if not menu_visible:
            return False
        if getattr(self, '_update_check_deferred_from_menu', False):
            return True
        self._update_check_deferred_from_menu = True
        try:
            if not bool(getattr(menu, '_closing', False)):
                menu.close()
        except Exception:
            pass

        def _resume(remaining: int = 8):
            if getattr(self, '_destroyed', False):
                self._update_check_deferred_from_menu = False
                return
            try:
                still_visible = bool(menu is not None and getattr(menu, 'visible', False))
            except Exception:
                still_visible = False
            if still_visible and remaining > 0:
                try:
                    self.root.after(80, lambda: _resume(remaining - 1))
                    return
                except Exception:
                    pass
            self._update_check_deferred_from_menu = False
            callback()

        try:
            self.root.after(80, _resume)
        except Exception:
            self._update_check_deferred_from_menu = False
            callback()
        return True

    def _check_for_updates_interactive(self):
        if self._defer_update_check_until_menu_closed(self._run_update_check_interactive):
            return
        self._run_update_check_interactive()

    def _run_update_check_interactive(self):
        if getattr(self, '_destroyed', False):
            return
        try:
            from updater.sao_updater import (
                get_manager,
                STATE_AVAILABLE, STATE_UP_TO_DATE, STATE_READY,
                STATE_DOWNLOADING, STATE_ERROR,
            )
        except Exception as e:
            SAODialog.showinfo(self._float, '更新', f'更新模块不可用: {e}')
            return
        mgr = get_manager()
        st = mgr.snapshot()

        # 已就绪 -> 询问是否立即重启
        if st.state == STATE_READY:
            SAODialog.ask(self._float, '更新就绪',
                          f'v{st.latest_version} 已下载完成。\n现在重启应用更新?',
                          on_ok=self._apply_downloaded_update)
            return

        # 已知有新版本 -> 直接询问是否下载
        if st.state == STATE_AVAILABLE:
            self._prompt_update_available(st)
            return

        if st.state == STATE_DOWNLOADING:
            SAODialog.showinfo(self._float, '更新', f'正在下载 ({int(st.progress * 100)}%)...')
            return

        if getattr(self, '_manual_update_check_inflight', False):
            return
        self._manual_update_check_inflight = True

        def _on_status(snapshot):
            if snapshot.state == STATE_AVAILABLE:
                try:
                    self.root.after(0, lambda snap=snapshot: self._show_manual_update_result(snap))
                except Exception:
                    self._manual_update_check_inflight = False
                mgr.remove_listener(_on_status)
            elif snapshot.state == STATE_UP_TO_DATE:
                try:
                    self.root.after(0, lambda snap=snapshot: self._show_manual_update_result(snap))
                except Exception:
                    self._manual_update_check_inflight = False
                mgr.remove_listener(_on_status)
            elif snapshot.state == STATE_ERROR:
                try:
                    self.root.after(0, lambda snap=snapshot: self._show_manual_update_result(snap))
                except Exception:
                    self._manual_update_check_inflight = False
                mgr.remove_listener(_on_status)

        mgr.add_listener(_on_status)
        try:
            mgr.check_async()
        except Exception as e:
            self._manual_update_check_inflight = False
            try:
                mgr.remove_listener(_on_status)
            except Exception:
                pass
            SAODialog.showinfo(self._float, '更新', f'检查失败: 更新线程启动异常: {e}')
            return

    def _show_manual_update_result(self, snapshot):
        self._manual_update_check_inflight = False
        try:
            from updater.sao_updater import STATE_AVAILABLE, STATE_UP_TO_DATE, STATE_ERROR
        except Exception:
            STATE_AVAILABLE, STATE_UP_TO_DATE, STATE_ERROR = 'available', 'up_to_date', 'error'
        if snapshot.state == STATE_AVAILABLE:
            self._prompt_update_available(snapshot)
        elif snapshot.state == STATE_UP_TO_DATE:
            SAODialog.showinfo(
                self._float, '更新',
                f'已是最新版本 ({APP_VERSION_LABEL})')
        elif snapshot.state == STATE_ERROR:
            SAODialog.showinfo(
                self._float, '更新',
                f'检查失败: {snapshot.error}')

    def _prompt_update_available(self, snapshot):
        from updater.sao_updater import get_manager, STATE_READY, STATE_ERROR
        mgr = get_manager()
        notes = (snapshot.notes or '').strip()
        size_mb = (snapshot.size or 0) / 1024 / 1024
        body = f'发现新版本 v{snapshot.latest_version}'
        if size_mb > 0:
            body += f' ({size_mb:.1f} MB)'
        if notes:
            body += f'\n\n{notes[:300]}'
        if snapshot.force_required:
            body += '\n\n[强制更新] 必须升级才能继续使用'

        def _do_download():
            def _on_dl(snap):
                if snap.state == STATE_READY:
                    try:
                        self.root.after(0, lambda: SAODialog.ask(
                            self._float, '更新就绪',
                            f'v{snap.latest_version} 已下载. 现在重启应用?',
                            on_ok=self._apply_downloaded_update))
                    except Exception:
                        pass
                    mgr.remove_listener(_on_dl)
                elif snap.state == STATE_ERROR:
                    try:
                        self.root.after(0, lambda: SAODialog.showinfo(
                            self._float, '更新', f'下载失败: {snap.error}'))
                    except Exception:
                        pass
                    mgr.remove_listener(_on_dl)
            mgr.add_listener(_on_dl)
            mgr.download_async()

        if snapshot.force_required:
            # 强制: 只能更新或退出
            def _quit():
                self._on_close()
            SAODialog.ask(self._float, '强制更新—仅可选择 更新/退出', body,
                          on_ok=_do_download, on_cancel=_quit)
        else:
            SAODialog.ask(self._float, '发现更新', body,
                          on_ok=_do_download)
