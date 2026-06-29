# -*- coding: utf-8 -*-
"""
SAOPlayerGUIPanelsMixin — ninth mixin extracted from SAOPlayerGUI
(round 47 of the sao_gui split refactor). 11 methods, ~190 lines.

Combines the remaining panel-toggle helpers and small settings
toggles that didn't have a natural home in earlier mixins:

Panel visibility (1):
  * _toggle_hide_all_panels — one-shot withdraw / restore of every
    floating panel. Snapshots which panels were visible so restore can
    re-show only those.

Recognition (1):
  * _toggle_recognition_menu — menu entry that flips the
    recognition_active flag (or starts the engines on first toggle)

Settings toggles:
  * _toggle_sound_enabled — sound on/off (delegates to sao_sound)
  * _adj_sound_volume(delta) — sound volume +/-
  * _toggle_topmost — toggles -topmost on the float + status panels

Required SAOPlayerGUI attrs:
  * self._status_panel, self._update_panel
  * self._panels_hidden, self._hidden_panels_snapshot
  * self._float, self.root
  * self._recog_lock, self._recognition_active,
    self._recognition_engine, self._recognition_engines
  * self._fisheye_ov

Required SAOPlayerGUI methods (via MRO):
  * _dismiss_sao_menu_for_panel (Menu mixin)
  * _raise_panel_window (SAOPlayerGUI)
  * _maybe_stop_fisheye, _any_panel_open, _start_fisheye_overlay
    (Fisheye mixin)
  * _refresh_menu_if_open (Menu mixin)
  * _get_setting, _set_setting (SAOPlayerGUI)
    * _start_recognition,
    _update_status_panel (SAOPlayerGUI)
"""

from __future__ import annotations

import math
import threading
import time
from typing import Any, Optional

from utils.perf_probe import probe as _probe
from gui_modules.sao_gui_plugin_manager import PluginDetachedPanel, PluginManagerPanel
from gui_modules.sao_panel_ui import _set_sao_panel_theme


def _finite_int(value: Any, default: int = 0, *, lo: Optional[int] = None, hi: Optional[int] = None) -> int:
    try:
        num = float(default if value is None or value == '' else value)
    except Exception:
        num = float(default or 0)
    if not math.isfinite(num):
        num = float(default or 0)
    if lo is not None:
        num = max(float(lo), num)
    if hi is not None:
        num = min(float(hi), num)
    return int(num)


class SAOPlayerGUIPanelsMixin:
    """Mixin bundling plugin-panel visibility and settings toggles."""

    def _toggle_ai_editor_panel(self):
        """启动独立 AI Editor GUI 窗口 (pywebview)."""
        close_external = getattr(self, '_close_sao_menu_for_external_command', None)
        if callable(close_external):
            close_external()
        else:
            self._dismiss_sao_menu_for_panel()
        try:
            self._fisheye_close_suppress_until = time.time() + 1.4
        except Exception:
            pass
        try:
            self._destroy_fisheye_hit_layer()
        except Exception:
            pass
        try:
            self._release_fisheye_input_zorder(getattr(self, '_fisheye_ov', None))
        except Exception:
            pass
        try:
            self._stop_fisheye_overlay()
        except Exception:
            pass
        try:
            pending_until = float(getattr(self, '_ai_editor_open_pending_until', 0.0) or 0.0)
        except Exception:
            pending_until = 0.0
        now = time.time()
        if pending_until > now:
            return
        try:
            self._ai_editor_open_pending_until = now + 2.0
        except Exception:
            pass

        def _reset_ai_editor_pending() -> None:
            try:
                self._ai_editor_open_pending_until = 0.0
            except Exception:
                pass

        def _show_ai_editor_fallback_panel() -> None:
            try:
                if not self._ai_editor_panel:
                    from gui_modules.sao_gui_ai_editor import AIEditorPanel
                    self._ai_editor_panel = AIEditorPanel(self.root, self)
                if self._ai_editor_panel.is_visible():
                    self._ai_editor_panel.hide()
                else:
                    self._ai_editor_panel.show()
                    self.root.after(120, lambda: self._raise_panel_window(self._ai_editor_panel))
            except Exception:
                pass

        def _launch_ai_editor_worker() -> None:
            try:
                from ai_editor.app import launch
                launch(gui_ref=self)
            except Exception as exc:
                print(f"[AIEditor] launch failed: {exc}")
                try:
                    self.root.after(0, _show_ai_editor_fallback_panel)
                except Exception:
                    _show_ai_editor_fallback_panel()
            finally:
                try:
                    self.root.after(0, _reset_ai_editor_pending)
                except Exception:
                    _reset_ai_editor_pending()

        def _launch_ai_editor_after_menu() -> None:
            threading.Thread(
                target=_launch_ai_editor_worker,
                name="sao-ai-editor-menu-launch",
                daemon=True,
            ).start()

        try:
            self.root.after_idle(_launch_ai_editor_after_menu)
        except Exception:
            _launch_ai_editor_after_menu()

    def _toggle_process_selector_panel(self):
        self._dismiss_sao_menu_for_panel()
        if not getattr(self, '_process_selector_panel', None):
            from gui_modules.sao_gui_process_selector import ProcessSelectorPanel
            self._process_selector_panel = ProcessSelectorPanel(self.root, self)
        if self._process_selector_panel.is_visible():
            self._process_selector_panel.hide()
        else:
            # Process selector and fisheye are mutually exclusive
            try:
                self._stop_fisheye_overlay()
            except Exception:
                pass
            self._process_selector_panel.show()
            self.root.after(120, lambda: self._raise_panel_window(self._process_selector_panel))

    def _toggle_act_plugin_manager_panel(self):
        """打开/关闭插件管理面板 (tkinter)."""
        self._dismiss_sao_menu_for_panel()
        if not self._act_plugin_manager_panel:
            self._act_plugin_manager_panel = PluginManagerPanel(self.root, self)
            self._apply_act_panel_theme()
        if self._act_plugin_manager_panel.is_visible():
            self._act_plugin_manager_panel.hide()
        else:
            try:
                self._sao_panel_transition_until = 0.0
                self._destroy_fisheye_hit_layer()
                self._stop_fisheye_overlay()
            except Exception:
                pass
            self._act_plugin_manager_panel.show()
            self._apply_act_panel_theme()
            self.root.after(120, lambda: self._raise_panel_window(self._act_plugin_manager_panel))

    def _open_act_plugin_manager(self, tab='manage'):
        """打开插件管理面板并切到指定页签 (manage / panels)."""
        self._dismiss_sao_menu_for_panel()
        try:
            self._sao_panel_transition_until = 0.0
            self._destroy_fisheye_hit_layer()
            self._stop_fisheye_overlay()
        except Exception:
            pass
        self._ensure_plugin_window_bridge()
        if not self._act_plugin_manager_panel:
            self._act_plugin_manager_panel = PluginManagerPanel(self.root, self)
            self._apply_act_panel_theme()
        panel = self._act_plugin_manager_panel
        try:
            panel._active_tab = tab if tab in ('manage', 'panels') else 'manage'
        except Exception:
            pass
        panel.show()
        self._apply_act_panel_theme()
        self.root.after(120, lambda: self._raise_panel_window(panel))

    def _ensure_plugin_window_bridge(self):
        """订阅 ``plugin_open_window``：让插件用 ``ctx.open_window`` 弹出独立窗口。

        幂等。插件按钮动作在 Tk 主线程触发，发布的事件在此同线程回调；为稳妥仍
        marshal 到 ``root.after``。任何面板可见前都会先经过 manager / 分离面板 /
        popup 三个入口之一，故此处必被激活。
        """
        if getattr(self, '_plugin_window_bridge_token', None):
            return
        try:
            from act_platform.runtime import ensure_act_event_bus
            bus = ensure_act_event_bus(self)

            def _on_open(event):
                payload = (event or {}).get('payload') or {}
                pid = str(payload.get('plugin_id') or '')
                if not pid:
                    return
                panel_id = str(payload.get('panel_id') or '')
                w = _finite_int(payload.get('width'), 0, lo=0)
                h = _finite_int(payload.get('height'), 0, lo=0)
                try:
                    self.root.after(0, lambda: self._open_plugin_detached_panel(pid, panel_id, w, h))
                except Exception:
                    pass

            self._plugin_window_bridge_token = bus.subscribe(
                'plugin_open_window', _on_open, owner_id='entity_plugin_window_bridge')
        except Exception:
            self._plugin_window_bridge_token = ''

    def _open_plugin_detached_panel(self, plugin_id, panel_id='', width=0, height=0):
        """打开某插件某面板的独立窗口 (一个面板一个窗口；尺寸由插件声明，缺省则默认)."""
        self._dismiss_sao_menu_for_panel()
        self._ensure_plugin_window_bridge()
        panels = getattr(self, '_plugin_detached_panels', None)
        if panels is None:
            panels = {}
            self._plugin_detached_panels = panels
        key = (str(plugin_id or ''), str(panel_id or ''))
        panel = panels.get(key)
        if panel is None or not panel._exists():
            panel = PluginDetachedPanel(self.root, self, plugin_id,
                                        panel_id=str(panel_id or ''),
                                        width=_finite_int(width, 0, lo=0),
                                        height=_finite_int(height, 0, lo=0))
            panels[key] = panel
        panel.show()
        try:
            self.root.after(120, lambda: self._raise_panel_window(panel))
        except Exception:
            pass

    def _toggle_plugin_enabled(self, plugin_id, enabled):
        """启用/禁用某个插件 (供 popup 菜单调用)."""
        from act_platform.runtime import act_plugin_disable, act_plugin_enable
        try:
            if enabled:
                act_plugin_disable(self, plugin_id)
            else:
                act_plugin_enable(self, plugin_id)
        except Exception:
            pass
        panel = getattr(self, '_act_plugin_manager_panel', None)
        if panel is not None:
            try:
                panel.refresh()
            except Exception:
                pass
        try:
            self._refresh_menu_if_open()
        except Exception:
            pass

    def _show_plugin_popup_menu(self):
        """SAO 插件 popup 菜单 — 插件管理 + 各插件(打开面板/启停/置顶)."""
        import tkinter as tk

        from act_platform.runtime import act_plugin_menu
        self._dismiss_sao_menu_for_panel()
        try:
            data = act_plugin_menu(self)
        except Exception:
            data = {'ok': False, 'plugins': []}
        menu = tk.Menu(self.root, tearoff=0)
        menu.add_command(label='⚙ 插件管理面板 Manage',
                         command=lambda: self._open_act_plugin_manager('manage'))
        menu.add_command(label='⬢ 插件面板 Panels',
                         command=lambda: self._open_act_plugin_manager('panels'))
        menu.add_separator()
        items = list(data.get('plugins') or [])
        if not items:
            menu.add_command(label='(未发现插件 No plugins)', state='disabled')
        for it in items:
            pid = str(it.get('id') or '')
            name = str(it.get('label') or pid)
            enabled = bool(it.get('enabled'))
            pinned = bool(it.get('pinned'))
            sub = tk.Menu(menu, tearoff=0)
            sub.add_command(label=('禁用 Disable' if enabled else '启用 Enable'),
                            command=lambda p=pid, en=enabled: self._toggle_plugin_enabled(p, en))
            sub.add_command(label=('取消置顶 Unpin' if pinned else '置顶 Pin'),
                            command=lambda p=pid, pn=pinned: self._pin_plugin_from_menu(p, not pn))
            # Only plugins that DECLARE a panel get the open-panel entry —
            # opens that plugin's own detached panel (not the manager).
            if it.get('declares_panel'):
                sub.add_command(label='打开面板 Open panel',
                                command=lambda p=pid: self._open_plugin_detached_panel(p))
            prefix = ('★ ' if pinned else '') + ('● ' if it.get('active') else ('◐ ' if enabled else '○ '))
            menu.add_cascade(label=prefix + name, menu=sub)
        x = self.root.winfo_pointerx()
        y = self.root.winfo_pointery()
        menu.tk_popup(x, y)

    def _toggle_hide_all_panels(self):
        """一键隐藏/显示所有浮动面板 (不销毁, 只是 withdraw/deiconify)"""
        panels = []
        for attr in vars(self):
            if attr.endswith('_panel') and attr.startswith('_'):
                obj = getattr(self, attr, None)
                if obj is not None:
                    win = getattr(obj, '_win', obj)
                    panels.append((attr.strip('_'), win))

        if not self._panels_hidden:
            # ── 隐藏 ──
            self._hidden_panels_snapshot = []
            for name, p in panels:
                try:
                    if p and p.winfo_exists():
                        self._hidden_panels_snapshot.append(name)
                        p.withdraw()
                except Exception:
                    pass
            self._panels_hidden = True
            self._maybe_stop_fisheye()
        else:
            # ── 恢复 ──
            for name, p in panels:
                try:
                    if name in self._hidden_panels_snapshot and p and p.winfo_exists():
                        p.deiconify()
                        p.lift()
                except Exception:
                    pass
            self._hidden_panels_snapshot = []
            self._panels_hidden = False
            # 面板恢复后确保鱼眼也恢复
            if self._fisheye_ov is None and self._any_panel_open():
                self.root.after(100, self._start_fisheye_overlay)

        self._refresh_menu_if_open()

    # ── Panel Theme ──

    _PLATFORM_PANEL_ATTRS = (
        '_act_plugin_manager_panel',
        '_process_selector_panel',
    )

    def _act_panel_theme(self) -> str:
        try:
            cfg = self._cfg_settings_ref or self.settings
            themes = dict(cfg.get('panel_themes', {}) or {})
            return 'light' if themes.get('plugin_manager') == 'light' else 'dark'
        except Exception:
            return 'dark'

    def _apply_act_panel_theme(self, theme: Optional[str] = None) -> None:
        """Apply the grouped panel theme to all registered Tk plugin panels."""
        theme = 'light' if str(theme or self._act_panel_theme()).lower() == 'light' else 'dark'
        try:
            _set_sao_panel_theme(theme, repaint_registered=True)
        except Exception:
            pass
        for attr in self._PLATFORM_PANEL_ATTRS:
            panel = getattr(self, attr, None)
            if panel is None:
                continue
            refresh = getattr(panel, 'refresh_theme', None)
            if callable(refresh):
                try:
                    refresh()
                except Exception:
                    pass
                continue
            win = getattr(panel, '_win', None)
            try:
                if win and win.winfo_exists():
                    _set_sao_panel_theme(theme, win)
            except Exception:
                pass

    def _toggle_recognition_menu(self):
        """切换插件识别开关。"""
        with self._recog_lock:
            if not self._recognition_active:
                if not self._recognition_engine and not self._recognition_engines:
                    self._start_recognition()
                else:
                    self._recognition_active = True
            else:
                self._recognition_active = False
        notifier = getattr(self, '_notify_plugin_menu_surfaces', None)
        if callable(notifier):
            notifier('on_recognition_changed')
        self._refresh_menu_if_open()

    def _toggle_sound_enabled(self):
        from utils.sao_sound import set_sound_enabled, get_sound_enabled
        new = not get_sound_enabled()
        set_sound_enabled(new)
        self._set_setting('sound_enabled', new)
        self._refresh_menu_if_open()

    def _adj_sound_volume(self, delta: int):
        from utils.sao_sound import set_sound_volume, get_sound_volume
        cur = get_sound_volume()
        nv = max(0, min(100, cur + delta))
        set_sound_volume(nv)
        self._set_setting('sound_volume', nv)

    # ══════════════════════════════════════════════
    #  其他功能
    # ══════════════════════════════════════════════
    def _toggle_topmost(self):
        current = self._float.attributes('-topmost')
        new_val = not current
        self._float.attributes('-topmost', new_val)
        for panel in [getattr(self, '_status_panel', None), getattr(self, '_update_panel', None)]:
            try:
                if panel and panel.winfo_exists():
                    panel.attributes('-topmost', new_val)
            except Exception:
                pass
        self._refresh_menu_if_open()
