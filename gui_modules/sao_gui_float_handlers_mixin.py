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
  * _arm_pending_combat_reset(scene_event=None) — defer same-
    instance combat-encounter DPS reset until the next real
    damage tick after a scene event.
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
  * _toggle_auto_script, _toggle_boss_raid,
    _boss_raid_next_phase (Actions mixin)
  * _show_plugin_popup_menu (Panels mixin)
"""

from __future__ import annotations

import ctypes
import math
import tkinter as tk
import time
from typing import Any, Optional

from utils.sao_sound import get_cjk_font
from gui_modules.sao_hotkey_manager import SAOHotkeyManager
from gui_modules.sao_panel_ui import (
    _apply_window_icon, _disable_native_window_shadow,
)


# Win32 user32 (used by _create_floating_widget's WS_EX flags). Mirrors
# the same _user32 handle that sao_panel_ui uses; ctypes.windll caches
# module handles so both modules share the same object.
_user32 = ctypes.windll.user32


def _finite_float(value: Any, default: float = 0.0, *, lo: Optional[float] = None) -> float:
    try:
        num = float(default if value is None or value == '' else value)
    except Exception:
        num = float(default or 0.0)
    if not math.isfinite(num):
        num = float(default or 0.0)
    if lo is not None:
        num = max(float(lo), num)
    return num


class SAOPlayerGUIFloatHandlersMixin:
    """Mixin bundling float-button drag handlers + small misc helpers."""

    def _arm_pending_combat_reset(self, scene_event=None):
        """Defer same-instance encounter reset until the next real damage."""
        reason = 'restart'
        delay_s = 3.0
        if isinstance(scene_event, dict):
            reason = str(scene_event.get('reason') or scene_event.get('kind') or reason)
            delay_s = _finite_float(scene_event.get('reset_delay_s'), delay_s, lo=0.0)
        self._pending_combat_reset_after = time.time() + max(0.0, delay_s)
        self._pending_combat_reset_reason = reason
        try:
            mgr = getattr(self, '_encounter_mgr', None)
            if mgr is not None:
                mgr.arm_pending_reset(reason, delay_s=delay_s)
        except Exception:
            pass
        self._scene_damage_grace_until = max(
            _finite_float(getattr(self, '_scene_damage_grace_until', 0.0), 0.0, lo=0.0),
            time.time() + max(8.0, delay_s + 8.0),
        )
        self._last_boss_hp_push_sig = None
        try:
            if self._dps_tracker:
                self._dps_tracker.invalidate_snapshot_cache()
        except Exception:
            pass
        print(
            f'[SAO Entity] ♻ 同副本重开候选({reason}) — 等下一次伤害再重置 DPS/BossHP',
            flush=True,
        )

    # Round 76 of sao_gui split refactor: _float_click / _float_drag /
    # _float_release were dead code. _create_floating_widget binds the
    # float button's <Button-1> directly to _toggle_sao_menu (round 56);
    # these three handlers were never wired to a Tk event and they
    # referenced self._drag which was never initialized — confirmed via
    # pre-refactor git blame (always dead, predates the split).
    # Removed to fix the latent AttributeError + clear the noise.

    def _float_enter(self, e):
        """高亮悬浮 HP 组件"""
        try:
            self._hp_hover = True
            self._float_alpha = 1.0
            self._refresh_hp_layered()
        except Exception:
            pass

    def _float_leave(self, e):
        """恢复默认色"""
        try:
            self._hp_hover = False
            self._float_alpha = 1.0   # 完全不透明 — 覆盖游戏原生条
            self._refresh_hp_layered()
        except Exception:
            pass

    def _lift_float_loop(self):
        """SAO 菜单开启时持续将悬浮按钮保持在最上层.

        v3.1.9 round 22: cadence bumped from 150 ms (6.7 Hz) to 250 ms
        (4 Hz). The float button is the small floating HP/status badge
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
        except Exception:
            pass
        try:
            self.root.after(250, self._lift_float_loop)
        except Exception:
            pass

    # ── SAO 菜单 session helpers moved to gui_modules/sao_gui_session_mixin.py
    # (round 31 of the sao_gui split refactor). The methods
    #   _session_int / _session_self_uid / _merge_session_player /
    #   _sync_session_players_cache / _format_session_power /
    #   _get_session_player_rows / _refresh_session_players_panel /
    #   _toggle_session_players_panel
    # all come from the SAOPlayerGUISessionMixin parent class.


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
        # Many ACT/plugin panels briefly demote themselves at ~220ms after
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
            'toggle_auto_script': lambda: self.root.after(0, self._toggle_auto_script),
            'hide_panels': lambda: self.root.after(0, self._toggle_hide_all_panels),
            'show_plugins': lambda: self.root.after(0, self._show_plugin_popup_menu),
            'boss_raid_start': lambda: self.root.after(0, self._toggle_boss_raid),
            'boss_raid_next_phase': lambda: self.root.after(0, self._boss_raid_next_phase),
            'toggle_auto_dodge': lambda: self.root.after(0, self._toggle_auto_dodge),
        }, hotkey_provider=self._plugin_hotkey_map)

    def _toggle_auto_dodge(self):
        """紧急停用/恢复自动躲避总开关 (默认 F12)。"""
        try:
            from engines.boss_autokey_linkage import load_linkage_config, set_dodge_enabled
            cfg = load_linkage_config(self._cfg_settings_ref)
            new_state = not bool(cfg.get('dodge_enabled', True))
            set_dodge_enabled(self._cfg_settings_ref, new_state)
            # 急停: 立刻松开定向躲避按住的所有 WASD 键, 杀掉在途位移
            director = getattr(self, '_auto_dodge_director', None)
            if director is not None:
                try:
                    director.release_all()
                except Exception:
                    pass
            key = 'F12'
            try:
                v = (self.settings.get('hotkeys', {}) or {}).get('toggle_auto_dodge')
                if isinstance(v, dict):
                    v = v.get('key') or v.get('name')
                if v:
                    key = str(v).upper()
            except Exception:
                pass
            msg = (f'已启用 ({key} 紧急停用)' if new_state
                   else f'已禁用 ({key} 重新启用)')
            if getattr(self, '_alert_overlay', None):
                self._alert_overlay.show_alert('自动躲避', msg)
            # 同步刷新已打开的机制面板 — 否则总开关显示与引擎实际状态脱节
            for attr in ('_bossraid_panel', '_bossraid_detail_panel'):
                p = getattr(self, attr, None)
                if p is None:
                    continue
                try:
                    p._mech_bump()
                    if hasattr(p, '_mx_rerender'):
                        p._mx_rerender()
                except Exception:
                    pass
            print(f'[SAO Entity] auto-dodge {"on" if new_state else "off"}')
        except Exception as e:
            print(f'[SAO Entity] toggle_auto_dodge failed: {e}')

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
        """SAO 菜单点击锚点窗口 (不渲染 HP — HP 由 sao_gui_hp.HpOverlay 独立渲染).

        历史: 本窗口曾以 UpdateLayeredWindow + PIL 方式渲染一个 75% 屏宽
        的 HP HUD。该渲染已被 ``sao_gui_hp.HpOverlay`` 取代, 故本窗口现
        仅作为 SAOPopUpMenu 的 anchor_widget 和右键菜单宿主使用, 全透
        明但保留点击命中测试。
        """
        try:
            _sw = self.root.winfo_screenwidth()
            _sh = self.root.winfo_screenheight()
        except Exception:
            _sw, _sh = 1920, 1080

        # ── 锚点窗口尺寸 (保持旧 SAO 菜单定位): 75% 屏宽, 高 140px ──
        FW = int(_sw * 0.75)
        FH = 140
        self._fw, self._fh = FW, FH
        self._float_alpha = 0.0
        self._hp_hover = False

        # (legacy — no longer rendered here; kept for stub compatibility)
        self._hp_ox = int(FW * 0.44)
        self._hp_oy = 38
        self._id_plate_w = int(FW * 0.40)

        self._float = tk.Toplevel(self.root)
        self._float.overrideredirect(True)
        self._float.attributes('-topmost', True)
        self._float.geometry(f'{FW}x{FH}')
        # 完全透明点击锚点: Tk 的 -alpha=0.0 (底层 LWA_ALPHA=0) 在 Windows
        # 下窗口不可见, 但 WS_EX_LAYERED + 统一 alpha 模式下点击仍能命中
        # 窗口矩形 — 这正是我们希望的 (保留 SAO 菜单 anchor + 右键菜单).
        self._float.configure(bg='#000000')
        try:
            self._float.attributes('-alpha', 0.0)
        except Exception:
            pass
        _apply_window_icon(self._float)

        # ── 获取 HWND 仅用于 AppBar 样式 (不再用 ULW) ──
        self._float_hwnd = 0
        try:
            self._float.update_idletasks()
            GWL_EXSTYLE = -20
            WS_EX_APPWINDOW = 0x00040000
            WS_EX_TOOLWINDOW = 0x00000080
            hwnd = int(_user32.GetParent(ctypes.c_void_p(self._float.winfo_id())))
            self._float_hwnd = hwnd
            style = _user32.GetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE)
            style = (style | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW
            _user32.SetWindowLongW(ctypes.c_void_p(hwnd), GWL_EXSTYLE, style)
            _disable_native_window_shadow(self._float)
            try:
                _user32.SetWindowDisplayAffinity(ctypes.c_void_p(hwnd), 0x00000011)
            except Exception:
                pass
        except Exception:
            self._float_hwnd = 0

        # (占位符 — 旧 HP shell 缓存, 已弃用)
        self._hp_shell_normal = None
        self._hp_shell_hover = None

        # ── HP 布局常量 (保留 — 外部代码可能仍引用) ──
        ox, oy = self._hp_ox, self._hp_oy
        bar_x = ox + 110; bar_y = oy + 10
        PW, PT, PH, PS = 350, 19, 27, 145
        self._hp_bar_x        = bar_x + 2
        self._hp_bar_y        = bar_y + 2
        self._hp_bar_right    = bar_x + PW - 3
        self._hp_bar_bot_top  = bar_y + PT - 1
        self._hp_bar_bot_full = bar_y + PH - 2
        self._hp_bar_step_x   = bar_x + PS - 2

        # ── 显示名 ──
        display_name = self._username if self._username else 'Player'
        if len(display_name) > 10:
            display_name = display_name[:9] + '…'
        self._hp_display_name = display_name

        # ── 点击交互 (拖拽已禁用 — 固定位置) ──
        self._float.bind('<Button-1>', lambda e: self._toggle_sao_menu())
        self._float.bind('<Enter>', self._float_enter)
        self._float.bind('<Leave>', self._float_leave)

        # 右键菜单 (SAO Auto — 精简, 深色, 向上弹出)
        self._float_ctx = tk.Menu(self._float, tearoff=0,
                                  bg='#0f121a', fg='#d0e8f0',
                                  activebackground='#f3af12',
                                  activeforeground='#ffffff',
                                  relief='flat', bd=1,
                                  font=get_cjk_font(9))
        self._float_ctx.add_command(label='◆ 打开 SAO 菜单', command=self._toggle_sao_menu)
        self._float_ctx.add_separator()
        self._float_ctx.add_command(label='◉ 状态面板', command=self._toggle_status_panel)
        self._float_ctx.add_command(label='⚡ AutoKey Quick', command=self._toggle_autokey_panel)
        self._float_ctx.add_command(label='⚡ AutoKey Detail', command=self._toggle_autokey_detail_panel)
        self._float_ctx.add_command(label='⚔ BossRaid Quick', command=self._toggle_bossraid_panel)
        self._float_ctx.add_command(label='⚔ BossRaid Detail', command=self._toggle_bossraid_detail_panel)
        self._float_ctx.add_separator()
        self._float_ctx.add_command(label='◈ 隐藏/显示面板', command=self._toggle_hide_all_panels)
        self._float_ctx.add_command(label='◇ WebView UI', command=self._switch_to_webview_ui)
        self._float_ctx.add_command(label='✕ 退出', command=self._on_close)
        def _show_ctx_menu(e):
            self._ctx_menu_open = True
            try:
                # 在点击位置上方弹出菜单
                menu_h = 165
                popup_x = e.x_root
                popup_y = max(0, e.y_root - menu_h)
                self._float_ctx.tk_popup(popup_x, popup_y)
            except Exception:
                self._float_ctx.tk_popup(e.x_root, e.y_root)
            finally:
                self._ctx_menu_open = False
        self._float.bind('<Button-3>', _show_ctx_menu)

        # 初始渲染一次 (alpha=0，不可见)
        try:
            self._refresh_hp_layered()
        except Exception:
            pass

        # 初始隐藏 — LinkStart 完成后才显示
        self._float.withdraw()


    # ══════════════════════════════════════════════
    #  识别引擎
    # ══════════════════════════════════════════════
