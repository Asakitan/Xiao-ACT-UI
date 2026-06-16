# -*- coding: utf-8 -*-
"""
SAO Utils 风格完整 GUI — 独立 UI 壳

包含 SAO PopUpMenu 菜单系统, SAO Alert 对话框, HP 血条进度条,
LINK START 入场动画, SAO 风格文件选择器
"""

import tkinter as tk
from tkinter import ttk
import os
import sys
import json
import ctypes
import math
import time
import threading
from typing import Any, Dict, List, Optional, Tuple

# Round 77 of sao_gui split refactor: PIL / numpy / render_capture_sync
# imports were all used by methods that have moved to mixins (or by the
# round-71-deleted _get_hp_pil_font). The mixins carry their own
# imports; sao_gui.py no longer needs them.
try:
    from render.gpu_capture import capture_monitor_bgr_for_point, ensure_session, get_latest_bgr
except Exception:
    capture_monitor_bgr_for_point = None  # type: ignore
    ensure_session = None  # type: ignore
    get_latest_bgr = None  # type: ignore

from config import (
    APP_VERSION_LABEL, WINDOW_TITLE, WINDOW_SIZE,
    DEFAULT_HOTKEYS,
    FONTS_DIR,
    SettingsManager,
    get_skill_slot_rects,
    resource_path,
)
from sao_theme import (
    SAOColors, SAOButton, SAOProgressBar, SAOTitleBar, SAODialog,
    SAOLeaderboardDialog,
    SAOStatusPill, SAOResizeGrip, SAOFilePicker, SAOSeparator,
    SAOPopUpMenu, SAOHPBar, SAOLinkStart, SAOCircleButton,
    Animator, lerp, lerp_color, ease_out, ease_in_out,
    _close_alert as _sao_close_dialog,
)
from utils.sao_sound import play_sound, LevelUpEffect, load_sao_fonts, get_sao_font, get_cjk_font
from gui_modules.sao_gui_plugin_manager import PluginManagerPanel
# Panel UI helpers (constants + builders) extracted in round 49 of the
# sao_gui split refactor. Re-import the names that the rest of sao_gui.py
# still uses at the module level — the helpers themselves now live in
# gui_modules.sao_panel_ui.
from gui_modules.sao_panel_ui import (
    _apply_panel_style, _hex_rgba, _make_panel_close_button,
    _sao_panel_header, _bind_panel_drag, _sao_panel_body,
    _sao_panel_hud_canvas, _sao_row, _sao_pill,
    # Round 59: 4 Win32 helpers moved out of sao_gui.py into sao_panel_ui.
    _get_icon_path, _apply_window_icon, _set_clickthrough_style,
    _disable_native_window_shadow,
    # Round 71: _set_process_app_id (Win32 taskbar AppUserModelID) moved
    # alongside the other Win32 helpers; was duplicated here + in
    # sao_webview.py before.
    _set_process_app_id,
    _SAO_PANEL_BG, _SAO_PANEL_HEADER_BG, _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_BORDER, _SAO_PANEL_ACCENT, _SAO_PANEL_GOLD,
    _SAO_PANEL_SEP, _SAO_PANEL_BODY_BG, _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
)
# Round 77: perf_probe (_probe / _phase_trace / _perf_gauge) and
# _sao_cy_uihelpers (_CY_UI) imports were used by methods now in mixins.
# Each mixin imports what it needs; sao_gui.py is import-clean.


# _SESSION_WHEEL_ROOTS / _dispatch_session_wheel moved into
# gui_modules.sao_session_players_panel along with SAOSessionPlayersPanel
# (round 26 of the sao_gui split refactor).

try:
    import pynput.keyboard as pynput_kb
    from pynput.keyboard import Key, KeyCode
    PYNPUT_HOTKEY_AVAILABLE = True
except Exception:
    pynput_kb = None
    Key = None
    KeyCode = None
    PYNPUT_HOTKEY_AVAILABLE = False


# ModernColors + SmoothButton classes purged in round 30 (sao_gui split
# refactor): both were defined here but never used anywhere in the repo
# (repo-wide grep confirmed no callers in .py / .cs / .spec / .bat / .json).
# Likely leftovers from an earlier UI iteration.
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'settings.json')

# pyglet Link Start 渲染器 (已弃用, 保留文件但不再使用)
# OpenGL 上下文要求主线程, 与 tkinter 冲突, 改用 Canvas SAO-UI 隧道模型
HAS_PYGLET = False

# ── 全局快捷键检测 (复用 gui.py 逻辑) ──
def _is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False

KEYBOARD_HOTKEY_AVAILABLE = False
KEYBOARD_ERROR_MSG = None
try:
    import keyboard as kb
    try:
        _tc = lambda: None
        kb.add_hotkey('ctrl+alt+shift+f12', _tc, suppress=False)
        kb.remove_hotkey('ctrl+alt+shift+f12')
        KEYBOARD_HOTKEY_AVAILABLE = True
    except Exception as e:
        KEYBOARD_ERROR_MSG = str(e)
except ImportError:
    KEYBOARD_ERROR_MSG = "未安装keyboard库"

PYNPUT_HOTKEY_AVAILABLE = False
try:
    from pynput import keyboard as pynput_kb
    from pynput.keyboard import Key, KeyCode
    PYNPUT_HOTKEY_AVAILABLE = True
except ImportError:
    pass

GLOBAL_HOTKEY_AVAILABLE = PYNPUT_HOTKEY_AVAILABLE or KEYBOARD_HOTKEY_AVAILABLE


# SettingsManager 已迁移到 gui_modules/settings_manager.py
# (round 29 of the sao_gui split refactor — re-exported here so the rest
# of this module and external callers like main.py can still do
# `from sao_gui import SettingsManager`).
from gui_modules.settings_manager import SettingsManager  # noqa: E402


_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32
for _fn, _res, _args in [
    (_user32.GetDC, ctypes.c_void_p, [ctypes.c_void_p]),
    (_user32.ReleaseDC, ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p]),
    (_user32.GetParent, ctypes.c_void_p, [ctypes.c_void_p]),
    (_user32.GetWindowLongW, ctypes.c_long, [ctypes.c_void_p, ctypes.c_int]),
    (_user32.SetWindowLongW, ctypes.c_long, [ctypes.c_void_p, ctypes.c_int, ctypes.c_long]),
    (_user32.UpdateLayeredWindow, ctypes.c_int,
        [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
         ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong]),
    (_gdi32.CreateCompatibleDC, ctypes.c_void_p, [ctypes.c_void_p]),
    (_gdi32.CreateDIBSection, ctypes.c_void_p,
        [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
         ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_uint]),
    (_gdi32.SelectObject, ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_void_p]),
    (_gdi32.DeleteObject, ctypes.c_int, [ctypes.c_void_p]),
    (_gdi32.DeleteDC, ctypes.c_int, [ctypes.c_void_p]),
]:
    _fn.restype = _res
    _fn.argtypes = _args
del _fn, _res, _args



# SAOHotkeyManager 已迁移到 gui_modules/sao_hotkey_manager.py
# (round 25 of the sao_gui split refactor — keep the original name
# importable so external callers and the rest of this module continue
# to work without changes).
from gui_modules.sao_hotkey_manager import SAOHotkeyManager  # noqa: E402


# ══════════════════════════════════════════════════════════
#  SAO Player GUI — 完整独立 UI
# ══════════════════════════════════════════════════════════
#  SAO 左侧玩家信息面板 (替代 SAOLeftInfo)
#  对标 SAO-UI HP 组件 + LeftInfo 组件
# ══════════════════════════════════════════════════════════
# SAOPlayerPanel 已迁移到 gui_modules/sao_player_panel.py
# (round 27 post-cadence of the sao_gui split refactor).
from gui_modules.sao_player_panel import SAOPlayerPanel  # noqa: E402



# SAOSessionPlayersPanel 已迁移到 gui_modules/sao_session_players_panel.py
# (round 26 of the sao_gui split refactor — class + _SESSION_WHEEL_ROOTS
# registry + _dispatch_session_wheel helper all moved together).
from gui_modules.sao_session_players_panel import SAOSessionPlayersPanel  # noqa: E402




# SAOMenuLeftStack 已迁移到 gui_modules/sao_menu_left_stack.py
# (round 28 of the sao_gui split refactor).
from gui_modules.sao_menu_left_stack import SAOMenuLeftStack  # noqa: E402




# ══════════════════════════════════════════════════════════
#  SAO Player GUI — 纯悬浮 SAO Menu 架构
# ══════════════════════════════════════════════════════════
# v3.2.2 rounds 31-32: SAOPlayerGUI is split across mixins so this
# 7000+ line class can shrink. Mixins extracted so far:
#   - SAOPlayerGUISessionMixin (round 31) — SAO menu's in-session
#     player-roster helpers (8 methods, 208 lines).
#   - SAOPlayerGUIStateMixin   (round 32) — _recognition_loop +
#     _push_packet_overlays + _apply_fast_state_update +
#     _on_game_state_update (730 lines). Heart of the combat-lag work.
# More mixins (menu / fisheye / boss_raid / commander) will land in
# later rounds.
from gui_modules.sao_gui_session_mixin import SAOPlayerGUISessionMixin  # noqa: E402
# Game mixins (State/Actions/EngineToggles/DpsTheme) moved to plugin
from gui_modules.sao_gui_menu_mixin import SAOPlayerGUIMenuMixin  # noqa: E402
from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin  # noqa: E402
from gui_modules.sao_gui_panels_mixin import SAOPlayerGUIPanelsMixin  # noqa: E402
from gui_modules.sao_gui_status_updater_mixin import SAOPlayerGUIStatusUpdaterMixin  # noqa: E402
from gui_modules.sao_gui_dialogs_mixin import SAOPlayerGUIDialogsMixin  # noqa: E402
# EngineLifecycleMixin moved to plugin
# Game mixins (PacketCallbacksMixin/DamageEventsMixin) moved to plugin in 5.0.0
from gui_modules.sao_gui_float_hp_mixin import SAOPlayerGUIFloatHpMixin  # noqa: E402
from gui_modules.sao_gui_float_handlers_mixin import SAOPlayerGUIFloatHandlersMixin  # noqa: E402
from gui_modules.sao_gui_lifecycle_mixin import SAOPlayerGUILifecycleMixin  # noqa: E402
from gui_modules.sao_gui_panel_fx_mixin import SAOPlayerGUIPanelFxMixin  # noqa: E402
from gui_modules.sao_gui_link_animation_mixin import SAOPlayerGUILinkAnimationMixin  # noqa: E402
# SAOPlayerGUIDamageEventsMixin → moved to plugin
from gui_modules.sao_gui_misc_mixin import SAOPlayerGUIMiscMixin  # noqa: E402


class SAOPlayerGUI(SAOPlayerGUIMenuMixin, SAOPlayerGUIFisheyeMixin, SAOPlayerGUIPanelsMixin, SAOPlayerGUIStatusUpdaterMixin, SAOPlayerGUIDialogsMixin, SAOPlayerGUIFloatHpMixin, SAOPlayerGUIFloatHandlersMixin, SAOPlayerGUILifecycleMixin, SAOPlayerGUIPanelFxMixin, SAOPlayerGUILinkAnimationMixin, SAOPlayerGUIMiscMixin, SAOPlayerGUISessionMixin):
    """
    纯悬浮 SAO Utils 风格 GUI — 没有传统窗口！
    - 常驻: 小型悬浮触发按钮 (Toplevel)
    - 展开: SAO PopUpMenu 全屏菜单 = 主界面
    - 左面板: SAOPlayerPanel (玩家信息/赛季进度/状态)
    - 菜单按钮: 5 类 (控制/自动/Boss/面板/关于)
    - 子菜单: 实时工具与面板控制
    - 可选: 浮动钢琴/可视化面板
    """

    def __init__(self):
        _set_process_app_id('sao.auto.game.ui')
        self.root = tk.Tk()
        self.root.withdraw()  # root 永远隐藏, 只作为 Tk 事件循环
        self.root.title("SAO Auto — 游戏辅助 UI")

        self.settings = SettingsManager()
        # 记录当前 UI 模式 — 下次启动时使用
        self.settings.set('ui_mode', 'entity')
        self.settings.save()

        # ── 角色配置 (游戏插件 on_load 会覆盖 _username/_profession) ──
        self._username = ''
        self._profession = profile.get('profession', '')
        self._level = profile.get('level', 1)
        self._level_extra = 0
        self._season_exp = 0
        self._sta_offline_armed = False

        self._current_file = None
        self._panels_hidden = False  # 一键隐藏所有面板
        self._hidden_panels_snapshot = []  # 隐藏前记录哪些面板是开的
        self._player_panel = None  # 当 SAO 菜单打开时设置
        self._menu_left_stack = None
        self._session_players_panel = None
        self._session_players = {}
        self._session_players_self_uid = 0
        self._session_players_version = 0
        self._session_players_rows_cache_sig = None
        self._session_players_rows_cache = []
        self._session_players_last_sync_ts = 0.0
        self._last_session_players_panel_sig = None
        self._last_session_players_panel_push_ts = 0.0
        self._profile_dialog_pending = False
        self._profile_dialog_ref = None
        self._picker = None        # SAOFilePicker 引用 (防止 GC)
        self._status_panel = None  # 浮动状态面板
        self._update_panel = None  # 浮动更新面板
        self._update_snapshot = None
        self._update_listener_installed = False
        self._update_listener = None
        self._update_panel_hidden = False
        self._update_panel_state_key = ''
        self._update_popup_ready = False
        self._pending_update_popup_snapshot = None
        self._last_update_popup_key = ''
        self._menu_refresh_after_id = None
        self._menu_refresh_force = False
        self._menu_children_cache_sig = None
        self._menu_children_cache = None
        # v2.3.15: signature computation cache for menu refresh
        self._last_menu_refresh_sig = None
        self._last_menu_refresh_sig_time = 0.0
        self._cached_auto_key_result = None
        self._cached_auto_key_sig = None
        self._cached_boss_raid_result = None
        self._cached_boss_raid_sig = None
        self._fisheye_ov = None    # 菜单开启时的持久鱼眼叠加层
        self._fisheye_hit_layer = None
        self._ctx_menu_open = False  # 右键菜单弹出中, 暂停 z-order 置顶
        self._lift_loop_active = False
        self._skip_canvas_click = False
        self._float_progress_pct = 0.0
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []
        self._float_hud_ids = []
        self._float_hud_text = []
        self._destroyed = False  # hot-switch 守卫: 阻止 after() 回调在 root 销毁后执行
        self._exit_animating = False
        self._close_finalized = False
        self._sao_menu_close_pending = False
        self._entry_overlay = None
        self._exit_overlay = None
        # 浮动呼吸动画
        self._breath_active = False
        self._breath_base_x = 0
        self._breath_base_y = 0
        self._breath_t0 = 0.0

        # ── 体力覆盖板 (stamina overlay) ──
        self._stamina_win = None
        self._stamina_hwnd = 0
        self._sta_w = 0
        self._sta_h = 0
        self._sta_hp = (0, 1)
        self._sta_sta = (0, 1)

        # ── 识别引擎 ──
        self._recognition_active = False
        self._recognition_engine = None
        self._recognition_engines = []
        self._packet_engine = None
        self._vision_engine = None
        self._vision_paused_for_death = False
        self._last_dead_state = False
        self._state_mgr = None
        self._game_state = None
        self._cfg_settings_ref = None
        self._cache_loop_stop = threading.Event()
        self._recog_lock = threading.Lock()

        # ── AutoKey / BossRaid / DPS 引擎 ──
        self._auto_key_engine = None
        self._boss_raid_engine = None
        self._boss_autokey_linkage = None
        self._dps_tracker = None
        self._dps_history_store = None
        self._encounter_mgr = None
        self._last_skill_event = {}
        self._last_dungeon_event = {}
        self._last_boss_event = {}
        self._dps_visible = False
        self._dps_enabled = True
        self._dps_faded = False
        self._dps_idle_reset_after_id = None
        self._dps_mode = 'hidden'
        self._dps_last_report_available = False
        self._last_burst_ready = False
        self._last_burst_slot = 0
        self._last_boss_timer_text = ''
        self._last_boss_timer_urgency = ''
        self._last_boss_bar_sig = None
        self._last_skillfx_sig = None
        self._profile_auto_saved = False
        self._last_gs_name = ''
        self._last_gs_prof = ''
        self._last_gs_uid = ''
        self._last_fast_state_sig = None
        self._last_player_panel_level_sig = None
        self._last_hp_overlay_hp_sig = None
        self._last_hp_overlay_sta_sig = None
        self._last_hp_sta_offline = None
        self._last_boss_hp_push_sig = None
        self._last_commander_push_sig = None
        self._panel_float_entries = {}
        self._panel_float_after_id = None

        # ── Boss bar target tracking (mirrors webview) ──
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._bb_last_hp_motion_sig = None
        self._bb_last_hp_motion_ts = 0.0
        self._bb_damage_timeout = 60.0
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        self._scene_damage_grace_until = time.time() + 20.0
        self._scene_hide_token = 0
        self._damage_self_fallback_log_ts = 0.0

        # ── ULW 覆盖层引用 ──
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._hp_overlay = None
        self._alert_overlay = None
        self._skillfx_overlay = None
        # 切换地图中央横幅 (延迟 3s 后淡入地图名); 去重状态见 _schedule_map_banner
        self._map_banner_overlay = None
        self._map_banner_last_name = ''
        self._map_banner_last_ts = 0.0
        self._map_banner_timer = None
        # v3.2.x: MemStateBridge (read-only Star.exe → GameState push).
        # Initialized in _start_recognition_engines once the data stack is
        # up; stopped in _stop_recognition_engines at top of method.
        self._mem_bridge = None
        self._act_trigger_engine = None
        self._skillfx_layout = None
        self._self_buff_overlay = None
        self._boss_buff_overlay = None
        # buffmon 数据缓存 — 由 _on_game_state_update / _on_monster_update 写入
        self._buffmon_target_uuid = 0

        # ── 配置面板实例 ──
        self._autokey_panel = None    # AutoKeyPanel
        self._bossraid_panel = None   # BossRaidPanel
        self._autokey_detail_panel = None
        self._bossraid_detail_panel = None
        self._commander_panel = None  # CommanderPanel
        self._act_plugin_manager_panel = None  # PluginManagerPanel
        self._act_trigger_timer_panel = None  # TriggerTimerManagerPanel
        self._act_data_source_health_panel = None  # DataSourceHealthPanel
        self._act_report_export_panel = None  # ReportExportPanel
        self._act_offline_import_panel = None  # OfflineImportPanel
        self._act_timeline_vcr_panel = None  # TimelineVcrPanel
        self._act_aggregate_panel = None  # ActAggregatePanel
        self._act_action_log_panel = None  # ActionLogPanel
        self._act_death_recap_panel = None  # DeathRecapPanel
        self._act_graph_timeseries_panel = None  # GraphTimeseriesPanel
        self._act_combatant_drilldown_panel = None  # CombatantDrilldownPanel
        self._act_skill_drilldown_panel = None  # SkillDrilldownPanel
        self._mem_scope_panel = None  # MemScopePanel
        self._ai_editor_panel = None  # AIEditorPanel
        self._commander_last_push = 0.0

        self._sao_menu = None  # lazy-init on first _toggle_sao_menu()
        self._set_icon()
        self._create_floating_widget()
        self._setup_hotkeys()
        self.root.after(0, self._ensure_updater_listener)

        # LINK START 入场
        self.root.after(100, self._play_link_start)

    def _set_setting(self, key: str, value):
        """Persist a setting to cfg_settings and save."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            self._cfg_settings_ref.set(key, value)
            try:
                self._cfg_settings_ref.save()
                self._setting_save_fail_at = 0.0
            except Exception as exc:
                # 设置在本会话内已生效, 只是没写进磁盘; 持续失败 (磁盘满/权限)
                # 时 60s 内只提示一次, 恢复成功后重新armed。
                now = time.monotonic()
                if now - getattr(self, '_setting_save_fail_at', 0.0) >= 60.0:
                    self._setting_save_fail_at = now
                    print(f'[SAO] settings save failed: {exc}')
                    try:
                        self._show_entity_alert(
                            '设置保存失败', f'{exc} — 本次修改重启后会丢失',
                            display_time=5.0)
                    except Exception:
                        pass

    def _get_setting(self, key: str, default=None):
        """Read a setting."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()
        # mainloop 已退出 — 先停 GLFW pump 线程（v2.3.14 解耦后 pump 在独立线程
        # 上跑 GL；必须在 root.destroy() 之前 join，否则 daemon 线程会被强杀，
        # 留下未释放的 WGL 上下文。
        try:
            from render import gpu_overlay_window as _gow
            if _gow._pump is not None:
                _gow._pump.shutdown()
        except Exception:
            pass
        # 清理 root 并处理热切换
        try:
            self.root.destroy()
        except Exception:
            pass
        if hasattr(self, '_after_shutdown') and self._after_shutdown:
            cb = self._after_shutdown
            self._after_shutdown = None
            try:
                cb()
            except Exception as e:
                print(f"[SAO] Hot switch: {e}")
                import traceback; traceback.print_exc()


def main():
    # 设置 DPI 感知 — 减少 Tkinter 控件锯齿 / 模糊
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
    app = SAOPlayerGUI()
    app.run()


if __name__ == "__main__":
    main()
