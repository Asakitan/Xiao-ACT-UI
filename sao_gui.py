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

from PIL import Image, ImageDraw, ImageTk, ImageFilter, ImageFont
import numpy as np
from render_capture_sync import wait_until_capture_idle
try:
    from gpu_capture import capture_monitor_bgr_for_point, ensure_session, get_latest_bgr
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
from character_profile import (
    load_profile, save_profile, get_or_ask_profile,
    show_welcome_dialog, PROFESSION_LIST,
)
from sao_sound import play_sound, LevelUpEffect, load_sao_fonts, get_sao_font, get_cjk_font
from auto_key_engine import (
    AutoKeyEngine,
    build_auto_key_state,
    build_identity_state,
    default_upload_auth_state,
    load_auto_key_config,
    save_auto_key_config,
    snapshot_author_from_state,
)
from boss_raid_engine import (
    BossRaidEngine,
    build_boss_raid_state,
    load_boss_raid_config,
    save_boss_raid_config,
)
from boss_autokey_linkage import (
    BossAutoKeyLinkage,
    load_linkage_config,
    save_linkage_config,
)
from dps_tracker import DpsTracker
from gui_modules.sao_gui_dps import DpsOverlay
from gui_modules.sao_gui_bosshp import BossHpOverlay
from gui_modules.sao_gui_hp import HpOverlay
from gui_modules.sao_gui_alert import AlertOverlay
from gui_modules.sao_gui_skillfx import BurstReadyOverlay
from gui_modules.sao_gui_buffmon import SelfBuffOverlay, BossBuffOverlay
from gui_modules.sao_gui_autokey import AutoKeyPanel
from gui_modules.sao_gui_bossraid import BossRaidPanel
from gui_modules.sao_gui_commander import CommanderPanel
from gui_modules.sao_gui_profile_editors import AutoKeyDetailPanel, BossRaidDetailPanel
# Panel UI helpers (constants + builders) extracted in round 49 of the
# sao_gui split refactor. Re-import the names that the rest of sao_gui.py
# still uses at the module level — the helpers themselves now live in
# gui_modules.sao_panel_ui.
from gui_modules.sao_panel_ui import (
    _apply_panel_style, _hex_rgba, _make_panel_close_button,
    _sao_panel_header, _bind_panel_drag, _sao_panel_body,
    _sao_panel_hud_canvas, _sao_row, _sao_pill,
    _SAO_PANEL_BG, _SAO_PANEL_HEADER_BG, _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_BORDER, _SAO_PANEL_ACCENT, _SAO_PANEL_GOLD,
    _SAO_PANEL_SEP, _SAO_PANEL_BODY_BG, _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
)
from perf_probe import probe as _probe, phase as _phase_trace, gauge as _perf_gauge

# v2.4.31: high-frequency UI helpers live in cython.
import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


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


def _get_icon_path():
    p = resource_path('icon.ico')
    return p if os.path.exists(p) else None


def _set_process_app_id(app_id: str):
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
    except Exception:
        pass


def _apply_window_icon(win):
    icon_path = _get_icon_path()
    if not icon_path:
        return
    try:
        win.iconbitmap(default=icon_path)
        win.iconbitmap(icon_path)
    except Exception:
        pass
    try:
        win.update_idletasks()
        hwnd = int(_user32.GetParent(ctypes.c_void_p(win.winfo_id())))
        if not hwnd:
            return
        IMAGE_ICON = 1
        LR_LOADFROMFILE = 0x10
        LR_DEFAULTSIZE = 0x40
        WM_SETICON = 0x80
        hicon = _user32.LoadImageW(None, icon_path, IMAGE_ICON, 0, 0,
                                   LR_LOADFROMFILE | LR_DEFAULTSIZE)
        if hicon:
            if not hasattr(win, '_taskbar_hicons'):
                win._taskbar_hicons = []
            win._taskbar_hicons.append(hicon)
            _user32.SendMessageW(ctypes.c_void_p(hwnd), WM_SETICON, 0, hicon)
            _user32.SendMessageW(ctypes.c_void_p(hwnd), WM_SETICON, 1, hicon)
    except Exception:
        pass



def _set_clickthrough_style(win):
    """给装饰/条带窗口设置 Win32 透明点击穿透样式。"""
    try:
        user32 = ctypes.windll.user32
        GWL_EXSTYLE = -20
        WS_EX_LAYERED = 0x00080000
        WS_EX_TRANSPARENT = 0x00000020
        WS_EX_TOOLWINDOW = 0x00000080
        hwnd = user32.GetParent(win.winfo_id()) or win.winfo_id()
        style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        style |= (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)
        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style)
    except Exception:
        pass


def _disable_native_window_shadow(win):
    """关闭透明/异形窗口的系统矩形阴影，避免阴影落到错误区域。"""
    try:
        win.update_idletasks()
        hwnd = int(_user32.GetParent(ctypes.c_void_p(win.winfo_id())) or win.winfo_id())
        policy = ctypes.c_int(1)  # DWMNCRP_DISABLED
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 2, ctypes.byref(policy), 4)
    except Exception:
        pass


def _make_sao_panel_hud(parent, width: int, height: int, alpha: float = 0.18):
    """生成一个轻量 SAO HUD 画布，提供左右错层飘移装饰。"""
    cv = tk.Canvas(parent, width=width, height=height, bg=parent.cget('bg'),
                   highlightthickness=0, bd=0)
    cv.place(x=0, y=0, relwidth=1, relheight=1)
    cv.tk.call('lower', cv._w)
    return cv



# ══════════════════════════════════════════════════════════
#  Win32 per-pixel alpha 分层窗口 (UpdateLayeredWindow)
# ══════════════════════════════════════════════════════════

# (_update_layered_win + 4 ULW ctypes structs deleted in round 28 of the
# sao_gui split refactor — the function was unused (other ULW consumers
# like sao_gui_bosshp / sao_gui_dps / overlay_render_worker have their
# own copies). _user32/_gdi32 signature setup above stays since 32+
# call sites in sao_gui still use those.)


# Win32 函数签名 (64-bit 安全, 防止 HWND/HDC 截断)
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


def _get_hp_pil_font(size, family='sao', _cache={}):
    """加载 PIL 字体用于 HP 条渲染 (带缓存)。"""
    key = (family, size)
    if key in _cache:
        return _cache[key]
    fname = 'SAOUI.ttf' if family == 'sao' else 'ZhuZiAYuanJWD.ttf'
    fp = os.path.join(FONTS_DIR, fname)
    try:
        font = ImageFont.truetype(fp, size=size)
    except Exception:
        font = ImageFont.load_default()
    _cache[key] = font
    return font


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
from gui_modules.sao_gui_state_mixin import SAOPlayerGUIStateMixin  # noqa: E402
from gui_modules.sao_gui_menu_mixin import SAOPlayerGUIMenuMixin  # noqa: E402
from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin  # noqa: E402
from gui_modules.sao_gui_actions_mixin import SAOPlayerGUIActionsMixin  # noqa: E402
from gui_modules.sao_gui_engine_toggles_mixin import SAOPlayerGUIEngineTogglesMixin  # noqa: E402
from gui_modules.sao_gui_dps_theme_mixin import SAOPlayerGUIDpsThemeMixin  # noqa: E402
from gui_modules.sao_gui_panels_mixin import SAOPlayerGUIPanelsMixin  # noqa: E402
from gui_modules.sao_gui_status_updater_mixin import SAOPlayerGUIStatusUpdaterMixin  # noqa: E402


class SAOPlayerGUI(SAOPlayerGUIMenuMixin, SAOPlayerGUIFisheyeMixin, SAOPlayerGUIActionsMixin, SAOPlayerGUIEngineTogglesMixin, SAOPlayerGUIDpsThemeMixin, SAOPlayerGUIPanelsMixin, SAOPlayerGUIStatusUpdaterMixin, SAOPlayerGUIStateMixin, SAOPlayerGUISessionMixin):
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

        # ── 角色配置 ──
        profile = load_profile()
        self._username = profile.get('username', '')
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
        self._hide_seek_engine = None
        self._hide_seek_alert_after_id = None
        self._hide_seek_alert_active = False

        # ── ULW 覆盖层引用 ──
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._hp_overlay = None
        self._alert_overlay = None
        self._skillfx_overlay = None
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
        self._commander_last_push = 0.0

        self._sao_menu = None  # lazy-init on first _toggle_sao_menu()
        self._set_icon()
        self._create_floating_widget()
        self._setup_hotkeys()
        self.root.after(0, self._ensure_updater_listener)

        # LINK START 入场
        self.root.after(100, self._play_link_start)

    def _set_icon(self):
        _set_process_app_id('sao.auto.game.ui')
        _apply_window_icon(self.root)
        # icon.ico 应用到 root (所有子窗口自动继承)

    def _create_hp_alpha_strip_windows(self):
        """(ULW 模式下 HP 填充已由 PIL alpha 梯度渲染, 不再需要条带窗口)"""
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []

    def _destroy_hp_alpha_strip_windows(self):
        for item in getattr(self, '_hp_alpha_windows', []):
            try:
                item['win'].destroy()
            except Exception:
                pass
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []

    def _render_hp_strip_image(self, *a, **kw):
        return None

    def _sync_hp_alpha_strip_windows(self):
        """(ULW 模式下不需要同步条带窗口)"""
        pass

    def _build_float_hud_items(self):
        """(ULW 模式下 HUD 已统一由 PIL 渲染, 此方法保留接口兼容)"""
        pass

    def _render_hp_shell(self, hover=False, scale=4):
        """(deprecated) HP 外壳已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _render_hp_dynamic(self):
        """(deprecated) HP 动态内容已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _refresh_hp_layered(self):
        """(deprecated) 旧 75%屏宽 HP ULW 已移除, 此方法为兼容占位。"""
        return

    def _set_float_alpha(self, alpha):
        """(deprecated) _float 现为全透明点击锚点, 不再需要 alpha。"""
        self._float_alpha = alpha

    def _animate_float_hud(self):
        """(deprecated) 旧 30fps HP 重绘循环已移除 (HpOverlay 自管帧率)。"""
        return

    # v2.3.15: 共享 HUD 装饰动画调度器 — 所有面板共用一个 after(66) 循环，
    # 替代每个面板独立 after(33)。配合签名缓存，只有坐标真正变化时才重建 Canvas。
    _sao_fx_panels = []       # [(panel, body_cv, pw, ph)] — 活跃面板列表
    _sao_fx_after_id = None   # 共享 after ID

    def _attach_sao_panel_fx(self, panel, header, inner, accent='#86dfff'):
        """给 Tk 浮动面板附加 SAO 风格 HUD 装饰 (共享调度 + 签名缓存).

        v2.3.15 优化:
        - 所有面板共用一个 after(66) 循环 (合并去重)
        - Canvas 内容按坐标签名缓存, 整数坐标不变时跳过 delete+rebuild
        - 从 33ms/panel → 66ms/global, 多面板时主线程开销从 O(N) 降到 O(1)
        """
        try:
            panel.update_idletasks()
            pw = max(80, panel.winfo_width())
            ph = max(60, panel.winfo_height())
        except Exception:
            return

        if getattr(panel, '_sao_fx_inited', False):
            return
        panel._sao_fx_inited = True
        header_cv = _make_sao_panel_hud(header, pw, 24)
        body_cv = _make_sao_panel_hud(inner, pw, ph)
        panel._sao_header_hud = header_cv
        panel._sao_body_hud = body_cv

        # 签名缓存: 上次绘制的坐标元组, 不变时跳过
        panel._sao_fx_last_sig = None
        cyan = '#86dfff'
        gold = '#f3af12'
        panel._sao_fx_items = {
            'lines': [
                body_cv.create_line(0, 0, 0, 0, fill=cyan, width=1),
                body_cv.create_line(0, 0, 0, 0, fill=gold, width=1),
                body_cv.create_line(0, 0, 0, 0, fill=cyan, width=1),
                body_cv.create_line(0, 0, 0, 0, fill=gold, width=1),
            ],
            'ticks': [
                (
                    body_cv.create_line(0, 0, 0, 0, fill=cyan, width=1),
                    body_cv.create_line(0, 0, 0, 0, fill=gold, width=1),
                )
                for _ in range(5)
            ],
            'rects': [
                body_cv.create_rectangle(0, 0, 0, 0, outline=cyan, width=1),
                body_cv.create_rectangle(0, 0, 0, 0, outline=gold, width=1),
            ],
        }

        # 注册到共享列表
        SAOPlayerGUI._sao_fx_panels.append((panel, body_cv, pw, ph))

        # 面板销毁时自动移除
        def _on_destroy(event=None):
            SAOPlayerGUI._sao_fx_panels[:] = [
                (p, cv, w, h) for p, cv, w, h in SAOPlayerGUI._sao_fx_panels
                if p is not panel
            ]
        panel.bind('<Destroy>', _on_destroy, add='+')

        # 启动共享调度 (仅当首次注册时)
        if SAOPlayerGUI._sao_fx_after_id is None:
            SAOPlayerGUI._sao_fx_shared_tick(self)

    @staticmethod
    @_probe.decorate('ui.sao_fx_shared_tick')
    def _sao_fx_shared_tick(self_ref):
        """共享 HUD 装饰 tick — 66ms 一次驱动所有面板.

        比旧方案 (每面板独立 after(33)) 省 N-1 个 after 回调.
        签名缓存确保仅坐标变化时才执行 Canvas 操作.
        """
        if self_ref._destroyed or not SAOPlayerGUI._sao_fx_panels:
            SAOPlayerGUI._sao_fx_after_id = None
            return

        tt = time.time()
        cyan = '#86dfff'
        gold = '#f3af12'
        active_count = 0

        for panel, body_cv, pw, ph in SAOPlayerGUI._sao_fx_panels:
            try:
                if (not panel.winfo_exists()
                        or not panel.winfo_viewable()
                        or not body_cv.winfo_exists()):
                    continue
            except Exception:
                continue
            active_count += 1

            # 每面板加一个相位偏移避免完全同步；坐标计算在 Cython。
            left_far, left_near, right_far, right_near = _CY_UI.sao_fx_coords(
                tt, id(panel), pw)

            # 签名: 所有可能变化的整数坐标
            sig = (left_far, left_near, right_far, right_near)
            if sig == panel._sao_fx_last_sig:
                continue  # 没有变化, 跳过 Canvas 操作
            panel._sao_fx_last_sig = sig

            # 有变化才移动现有 Canvas items，避免 delete + rebuild。
            items = getattr(panel, '_sao_fx_items', None)
            if not items:
                continue
            lines = items.get('lines') or []
            ticks = items.get('ticks') or []
            rects = items.get('rects') or []
            try:
                body_cv.coords(lines[0], left_far, 30, left_far + 78, 30)
                body_cv.coords(lines[1], left_near, ph - 44,
                               left_near + 102, ph - 44)
                body_cv.coords(lines[2], right_far - 88, 42, right_far, 42)
                body_cv.coords(lines[3], right_near - 110, ph - 58,
                               right_near, ph - 58)
                for i, pair in enumerate(ticks):
                    lx = left_far + i * 12
                    rx2 = right_far - i * 13
                    body_cv.coords(pair[0], lx, 48, lx, 54 + (i % 2) * 3)
                    body_cv.coords(pair[1], rx2, ph - 74,
                                   rx2, ph - 68 - (i % 2) * 3)
                body_cv.coords(rects[0], left_near + 8, ph - 36,
                               left_near + 66, ph - 24)
                body_cv.coords(rects[1], right_near - 74, 22,
                               right_near - 12, 34)
            except Exception:
                pass

        _perf_gauge('ui.sao_fx.active_panels', active_count)
        try:
            delay_ms = 90 if active_count > 0 else 250
            SAOPlayerGUI._sao_fx_after_id = self_ref.root.after(
                delay_ms, lambda: SAOPlayerGUI._sao_fx_shared_tick(self_ref))
        except Exception:
            SAOPlayerGUI._sao_fx_after_id = None

    # ══════════════════════════════════════════════
    #  悬浮触发按钮 — 纯 SAO-UI HP 组件 (对标 HP/src/index.vue)
    # ══════════════════════════════════════════════
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
    def _stop_recognition_engines(self):
        """停止所有识别/数据引擎."""
        if getattr(self, '_auto_key_engine', None):
            try: self._auto_key_engine.stop()
            except Exception: pass
            self._auto_key_engine = None
        if getattr(self, '_boss_raid_engine', None):
            try: self._boss_raid_engine.stop()
            except Exception: pass
            self._boss_raid_engine = None
        self._hide_seek_alert_active = False
        aid = getattr(self, '_hide_seek_alert_after_id', None)
        if aid is not None:
            try: self.root.after_cancel(aid)
            except Exception: pass
            self._hide_seek_alert_after_id = None
        if getattr(self, '_hide_seek_engine', None):
            try: self._hide_seek_engine.stop()
            except Exception: pass
            self._hide_seek_engine = None
        engines = list(getattr(self, '_recognition_engines', []) or [])
        if not engines and self._recognition_engine:
            engines = [self._recognition_engine]
        for engine in engines:
            try: engine.stop()
            except Exception: pass
        self._recognition_engines = []
        self._recognition_engine = None
        self._packet_engine = None
        self._vision_engine = None
        self._reset_sta_offline_state()

    def _reconfigure_data_engines(self):
        """重启 packet/vision 引擎以匹配当前数据源配置."""
        if not getattr(self, '_cfg_settings_ref', None) or not getattr(self, '_state_mgr', None):
            return
        self._stop_recognition_engines()

        engines = []
        try:
            from packet_bridge import PacketBridge
            # Phase 7: Entity menu reads `mem_data_source` preference
            # (key intentionally distinct from legacy 'data_source' which is
            # a packet-vs-vision toggle). Values: 'tcp' (default) | 'memory' |
            # 'hybrid' | 'auto'. Routes through mem_probe.UnifiedDataSource
            # for non-tcp modes so Entity menu gets the exact same dataset
            # the webview menu does.
            try:
                _data_source_mode = str(
                    self._cfg_settings_ref.get('mem_data_source', 'tcp') or 'tcp'
                ).lower()
            except Exception:
                _data_source_mode = 'tcp'
            packet_engine = PacketBridge(self._state_mgr, self._cfg_settings_ref,
                                         on_damage=self._on_packet_damage,
                                         on_monster_update=self._on_monster_update,
                                         on_boss_event=self._on_boss_event,
                                         on_scene_change=self._on_scene_change,
                                         data_source=_data_source_mode)
            packet_engine.start()
            engines.append(packet_engine)
            self._packet_engine = packet_engine
            print(f'[SAO Entity] Packet bridge started '
                  f'(data_source={_data_source_mode!r})')
        except Exception as e:
            import traceback
            print(f'[SAO Entity] Packet bridge FAILED: {e}')
            traceback.print_exc()
            self._packet_engine = None

        # DPS Tracker
        try:
            self._dps_tracker = DpsTracker()
            # Load skill name mapping (same as webview path)
            _skill_json = os.path.join(
                os.path.dirname(os.path.abspath(__file__)),
                'assets', 'skill_names.json',
            )
            if os.path.isfile(_skill_json):
                try:
                    with open(_skill_json, 'r', encoding='utf-8') as _sf:
                        _raw = json.load(_sf)
                    if isinstance(_raw, dict):
                        self._dps_tracker.set_skill_names(
                            {int(k): v for k, v in _raw.items()
                             if str(k).isdigit()}
                        )
                        print(f'[SAO Entity] Loaded {len(_raw)} skill names')
                except Exception as e:
                    print(f'[SAO Entity] skill_names.json load error: {e}')
            print('[SAO Entity] DPS tracker initialized')
        except Exception as e:
            print(f'[SAO Entity] DPS tracker init failed: {e}')
            self._dps_tracker = None

        try:
            from recognition import RecognitionEngine
            vision_engine = RecognitionEngine(self._state_mgr, self._cfg_settings_ref)
            vision_engine.start()
            engines.append(vision_engine)
            self._vision_engine = vision_engine
            print('[SAO Entity] Recognition engine started (window vision)')
        except Exception as e:
            import traceback
            print(f'[SAO Entity] Recognition engine FAILED: {e}')
            traceback.print_exc()
            self._vision_engine = None

        self._recognition_engines = engines
        self._recognition_engine = engines[0] if engines else None
        self._recognition_active = bool(engines)

    def _send_linked_key(self, key: str, press_mode: str = "tap",
                         hold_ms: int = 80, press_count: int = 1):
        """发送联动按键 (Boss→AutoKey linkage)."""
        try:
            from auto_key_engine import VK_NAME_MAP, INPUT, KEYBDINPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP
            import ctypes as _ct
            key = (key or "").strip().upper()
            vk = VK_NAME_MAP.get(key)
            if vk is None and len(key) == 1 and key.isalpha():
                vk = ord(key)
            if vk is None:
                return
            hold_s = max(0.015, hold_ms / 1000.0) if press_mode == "hold" else 0.015
            extra = _ct.c_ulong(0)
            for _ in range(max(1, press_count)):
                ki = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=0, time=0,
                                dwExtraInfo=_ct.pointer(extra))
                ev = INPUT(type=INPUT_KEYBOARD, ki=ki)
                _ct.windll.user32.SendInput(1, _ct.byref(ev), _ct.sizeof(INPUT))
                time.sleep(hold_s)
                ki2 = KEYBDINPUT(wVk=int(vk), wScan=0, dwFlags=KEYEVENTF_KEYUP, time=0,
                                 dwExtraInfo=_ct.pointer(extra))
                ev2 = INPUT(type=INPUT_KEYBOARD, ki=ki2)
                _ct.windll.user32.SendInput(1, _ct.byref(ev2), _ct.sizeof(INPUT))
                if press_count > 1:
                    time.sleep(0.04)
        except Exception as e:
            print(f"[Linkage] send_key error: {e}")

    def _arm_pending_combat_reset(self, scene_event=None):
        """Defer same-instance encounter reset until the next real damage."""
        reason = 'restart'
        delay_s = 3.0
        if isinstance(scene_event, dict):
            reason = str(scene_event.get('reason') or scene_event.get('kind') or reason)
            try:
                delay_s = float(scene_event.get('reset_delay_s', delay_s) or delay_s)
            except Exception:
                delay_s = 3.0
        self._pending_combat_reset_after = time.time() + max(0.0, delay_s)
        self._pending_combat_reset_reason = reason
        self._scene_damage_grace_until = max(
            float(getattr(self, '_scene_damage_grace_until', 0.0) or 0.0),
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

    def _maybe_apply_pending_combat_reset(self, event, is_self_combat_target: bool) -> bool:
        """Apply deferred encounter reset immediately before the first new hit."""
        try:
            reset_after = float(getattr(self, '_pending_combat_reset_after', 0.0) or 0.0)
        except Exception:
            reset_after = 0.0
        if reset_after <= 0.0 or time.time() < reset_after or not is_self_combat_target:
            return False
        try:
            damage = int(event.get('damage') or 0)
        except Exception:
            damage = 0
        if bool(event.get('is_heal', False)):
            return False
        if damage <= 0 and not (event.get('is_immune') or event.get('is_absorbed')):
            return False

        has_existing_encounter = False
        try:
            has_existing_encounter = bool(
                self._dps_tracker and self._dps_tracker.has_active_encounter())
        except Exception:
            has_existing_encounter = False
        if not has_existing_encounter:
            reason = str(getattr(self, '_pending_combat_reset_reason', '') or 'restart')
            self._pending_combat_reset_after = 0.0
            self._pending_combat_reset_reason = ''
            print(
                f'[SAO Entity] ♻ 延迟重置跳过: {reason} 当前遭遇战无有效伤害',
                flush=True,
            )
            return False

        reason = str(getattr(self, '_pending_combat_reset_reason', '') or 'restart')
        try:
            damage_ts = float(event.get('timestamp') or time.time())
        except Exception:
            damage_ts = time.time()
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        print(
            f'[SAO Entity] ♻ 下一次伤害到达，执行延迟重置: {reason}',
            flush=True,
        )

        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._bb_last_hp_motion_sig = None
        self._bb_last_hp_motion_ts = 0.0
        self._last_boss_hp_push_sig = None
        self._scene_damage_grace_until = damage_ts + 8.0
        try:
            if self._state_mgr:
                self._state_mgr.update(
                    boss_breaking_stage=-1,
                    boss_extinction_pct=0.0,
                    boss_current_hp=0,
                    boss_total_hp=0,
                    boss_hp_source='none',
                    boss_hp_est_pct=1.0,
                    boss_shield_active=False,
                    boss_shield_pct=0.0,
                    boss_in_overdrive=False,
                    boss_invincible=False,
                )
        except Exception:
            pass
        if self._dps_tracker:
            try:
                if self._dps_tracker.has_active_encounter():
                    self._dps_tracker.reset()
                else:
                    self._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
        self._dps_visible = False
        self._dps_faded = False
        self._dps_mode = 'hidden'
        self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
        if self._boss_raid_engine:
            try:
                if getattr(self._boss_raid_engine, '_state', '') != 'running':
                    self._boss_raid_engine.reset()
            except Exception:
                pass
        return True

    def _current_player_uid_int(self) -> int:
        for source in (
            getattr(self, '_game_state', None),
            getattr(getattr(self, '_state_mgr', None), 'state', None),
        ):
            try:
                uid = getattr(source, 'player_id', '') if source is not None else ''
                if str(uid).isdigit():
                    return int(uid)
            except Exception:
                pass
        try:
            return int(getattr(self, '_last_self_uid_pushed', 0) or 0)
        except Exception:
            return 0

    def _is_known_friendly_uid(self, uid: int) -> bool:
        uid = self._session_int(uid, 0)
        if uid <= 0:
            return False
        self_uid = self._current_player_uid_int()
        if self_uid > 0 and uid == self_uid:
            return True
        bridge = getattr(self, '_packet_engine', None)
        if bridge is not None:
            try:
                team_members = getattr(bridge, '_team_members', {}) or {}
                if uid in team_members:
                    return True
            except Exception:
                pass
            try:
                pdata = (bridge.get_players() or {}).get(uid)
            except Exception:
                pdata = None
            if pdata is not None:
                try:
                    if (getattr(pdata, 'name', '')
                            or int(getattr(pdata, 'level', 0) or 0) > 0
                            or int(getattr(pdata, 'profession_id', 0) or 0) > 0
                            or int(getattr(pdata, 'fight_point', 0) or 0) > 0):
                        return True
                except Exception:
                    pass
        entry = getattr(self, '_session_players', {}).get(uid)
        if entry is not None:
            try:
                return bool(
                    entry.get('is_self')
                    or entry.get('name')
                    or int(entry.get('fight_point') or 0) > 0
                    or int(entry.get('level') or 0) > 0
                    or int(entry.get('profession_id') or 0) > 0
                )
            except Exception:
                return False
        return False

    def _normalize_damage_event_for_self(self, event):
        if not isinstance(event, dict):
            return event
        self_uid = self._current_player_uid_int()
        if event.get('attacker_is_self'):
            try:
                attacker_uid = int(event.get('attacker_uid') or 0)
            except Exception:
                attacker_uid = 0
            if attacker_uid or self_uid <= 0:
                return event
            fixed = dict(event)
            fixed['attacker_uid'] = self_uid
            fixed.setdefault('self_uid', self_uid)
            return fixed
        if self_uid <= 0:
            return event

        def _event_int(name: str) -> int:
            try:
                return int(event.get(name) or 0)
            except Exception:
                return 0

        candidate_uids = [_event_int('attacker_uid')]
        for key in ('attacker_uuid', 'attacker_uuid_raw', 'top_summoner_id'):
            raw = _event_int(key)
            if raw and (raw & 0xFFFF) == 640:
                candidate_uids.append(raw >> 16)
            elif key == 'top_summoner_id' and raw:
                candidate_uids.append(raw)
        if self_uid not in candidate_uids:
            return event

        fixed = dict(event)
        fixed['attacker_is_self'] = True
        fixed['attacker_uid'] = self_uid
        fixed.setdefault('self_uid', self_uid)
        now = time.time()
        if now - float(getattr(self, '_damage_self_fallback_log_ts', 0.0) or 0.0) > 10.0:
            self._damage_self_fallback_log_ts = now
            print(
                f'[SAO Entity] 修正伤害归属: attacker_uid -> self_uid={self_uid}',
                flush=True,
            )
        return fixed

    def _normalize_damage_event_target_for_entity(self, event):
        """Recover combat-target classification after scene/dungeon switches.

        Some instance transitions clear the monster cache before the first
        damage packet of the next dungeon arrives. If that target UUID has a
        player-looking suffix, the parser can conservatively mark it as a
        player until SyncNearEntities catches up, which suppresses both DPS
        and BossHP. For self-outgoing damage to a target that is not present
        in the known player table, treat it as a combat target.

        v2.5.4: also force combat_target on by UUID encoding alone for any
        non-player-suffix target (StarResonanceDps / resonance-logs-cn parity).
        Stale `_players` entries preserved across scene resets used to make the
        `known_player` early return swallow the very first hit on a new boss
        whose encoded uid happened to collide with an old teammate's uid.
        """
        if not isinstance(event, dict):
            return event
        try:
            target_uuid = int(event.get('target_uuid') or 0)
        except Exception:
            target_uuid = 0
        if target_uuid <= 0:
            return event
        if event.get('target_is_combat_target') or event.get('target_is_monster'):
            return event

        target_suffix_player = (target_uuid & 0xFFFF) == 640
        if not target_suffix_player:
            # Pure UUID-encoding routing — no caches involved, so no stale
            # state can mis-flag a fresh boss after a map change.
            fixed = dict(event)
            fixed['target_is_player'] = False
            fixed['target_is_monster'] = True
            fixed['target_is_combat_target'] = True
            fixed['entity_target_fallback'] = 'uuid_suffix_combat_target'
            return fixed

        if not event.get('attacker_is_self'):
            return event

        target_uid = 0
        try:
            target_uid = target_uuid >> 16
        except Exception:
            target_uid = 0
        self_uid = self._current_player_uid_int()
        if target_uid and self_uid and target_uid == self_uid:
            return event
        if self._is_known_friendly_uid(target_uid):
            return event

        try:
            in_scene_damage_grace = time.time() < float(
                getattr(self, '_scene_damage_grace_until', 0.0) or 0.0)
        except Exception:
            in_scene_damage_grace = False
        if in_scene_damage_grace:
            try:
                damage = int(event.get('damage') or 0)
            except Exception:
                damage = 0
            if damage > 0 or event.get('is_immune') or event.get('is_absorbed'):
                fixed = dict(event)
                fixed['target_is_player'] = False
                fixed['target_is_monster'] = True
                fixed['target_is_combat_target'] = True
                fixed['entity_target_fallback'] = 'post_scene_self_damage_grace'
                return fixed

        known_player = False
        bridge = getattr(self, '_packet_engine', None)
        if bridge and target_uid:
            try:
                known_player = target_uid in (bridge.get_players() or {})
            except Exception:
                known_player = False
        if known_player:
            return event

        try:
            damage = int(event.get('damage') or 0)
        except Exception:
            damage = 0
        if damage <= 0 and not (event.get('is_immune') or event.get('is_absorbed')):
            return event

        fixed = dict(event)
        fixed['target_is_player'] = False
        fixed['target_is_monster'] = True
        fixed['target_is_combat_target'] = True
        fixed['entity_target_fallback'] = 'unknown_target_after_scene_change'
        return fixed

    def _on_packet_damage(self, event):
        """Damage event callback from packet_parser → boss raid engine + DPS tracker."""
        event = self._normalize_damage_event_for_self(event)
        event = self._normalize_damage_event_target_for_entity(event)
        # Track self -> non-player combat target damage for boss bar target.
        # BossHP only displays later if packet_parser has usable HP data.
        try:
            _target_uuid_for_boss_hp = int(event.get('target_uuid', 0) or 0)
        except Exception:
            _target_uuid_for_boss_hp = 0
        _is_friendly_boss_hp_target = bool(
            _target_uuid_for_boss_hp
            and self._is_known_friendly_uid(_target_uuid_for_boss_hp)
        )
        _is_self_combat_target = bool(
            event.get('attacker_is_self')
            and _target_uuid_for_boss_hp
            and not _is_friendly_boss_hp_target
            and (
                event.get('target_is_combat_target', False)
                or event.get('target_is_monster', False)
                or ('target_is_player' in event and not event.get('target_is_player', False))
            )
        )
        self._maybe_apply_pending_combat_reset(event, _is_self_combat_target)
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_damage_event(event)
            except Exception: pass
        if _is_self_combat_target:
            self._cancel_dps_idle_reset_after()
            target_uuid = event.get('target_uuid', 0)
            if target_uuid:
                damage_now = time.time()
                # ── Duplicate-UUID revive (v2.3.14) ─────────────────────
                # When a server reuses a monster UUID (mob respawn in
                # the same dungeon, or monster instance churn without a
                # NotifyStartPlayingDungeon), the parser still has the
                # old MonsterData with ``is_dead=True``. The recognition
                # loop filter below (``not getattr(_m, 'is_dead', False)``)
                # excludes it → BossHP panel never pops up. Damage
                # arriving for a UUID is proof that a live unit holds
                # that UUID right now, so clear the stale flag.
                _bridge = getattr(self, '_packet_engine', None)
                if _bridge is not None:
                    try:
                        _m = _bridge.get_monster(target_uuid)
                        if _m is not None and getattr(_m, 'is_dead', False):
                            _m.is_dead = False
                            _m.last_update = damage_now
                    except Exception:
                        pass
                self._bb_recent_targets[target_uuid] = damage_now
                self._bb_last_target_uuid = target_uuid
                self._bb_last_damage_ts = damage_now
                self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
                # Damage is also liveness. Some encounters lock HP while
                # still accepting hits, so do not let stable-HP hiding win.
                self._bb_last_hp_motion_ts = self._bb_last_damage_ts
                self._last_boss_hp_push_sig = None
                if self._dps_tracker:
                    try: self._dps_tracker.set_boss_uuid(target_uuid)
                    except Exception: pass
        if self._dps_tracker:
            try: self._dps_tracker.on_damage_event(event)
            except Exception: pass

    def _is_dead_state(self, gs) -> bool:
        if gs is None:
            return False
        # v2.4.31: predicate moved to cython.
        return bool(_CY_UI.is_dead_state(
            getattr(gs, 'hp_max', 0),
            getattr(gs, 'hp_current', 0),
            getattr(gs, 'hp_pct', 1.0),
        ))

    def _bump_boss_hp_target_hold(self, reason: str = ''):
        target_uuid = int(getattr(self, '_bb_last_target_uuid', 0) or 0)
        if not target_uuid:
            return
        bridge = getattr(self, '_packet_engine', None)
        if not bridge:
            return
        try:
            monster = bridge.get_monster(target_uuid)
            if not monster:
                return
            if not self._boss_monster_usable(monster):
                return
            now = time.time()
            self._bb_recent_targets[target_uuid] = now
            self._bb_last_damage_ts = now
            self._last_boss_hp_push_sig = None
        except Exception:
            pass

    def _boss_monster_usable(self, monster) -> bool:
        if not monster:
            return False
        # v2.4.31: pure predicate in cython; keep the side effect (UUID-reuse
        # revive) here so we don't pass the monster handle to native code.
        try:
            usable, revive = _CY_UI.boss_monster_usable(
                getattr(monster, 'hp', 0),
                getattr(monster, 'max_hp', 0),
                bool(getattr(monster, 'is_dead', False)),
            )
            if revive:
                monster.is_dead = False
                monster.last_update = time.time()
            return bool(usable)
        except Exception:
            return False

    def _sync_boss_hp_revive_hold(self, gs):
        dead_now = self._is_dead_state(gs)
        dead_prev = bool(getattr(self, '_last_dead_state', False))
        if (not dead_now) and dead_prev:
            self._bump_boss_hp_target_hold('revive')
        self._last_dead_state = dead_now

    def _on_monster_update(self, monster_data):
        """Monster update from packet_parser → boss raid engine + BossHP pretrack."""
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_monster_update(monster_data)
            except Exception: pass
        try:
            _uuid = int(monster_data.get('uuid', 0) or 0)
            _max_hp = int(monster_data.get('max_hp', 0) or 0)
            _hp = int(monster_data.get('hp', 0) or 0)
            _is_dead = bool(monster_data.get('is_dead', False))
            if (_uuid and _uuid == int(getattr(self, '_bb_last_target_uuid', 0) or 0)
                    and (_max_hp > 0 or _hp > 0) and (not _is_dead or _hp > 0)):
                self._last_boss_hp_push_sig = None
        except Exception:
            pass
        # Buff 监视器 (boss): 仅推当前锁定 boss target 的 buff_list
        try:
            ov = getattr(self, '_boss_buff_overlay', None)
            if ov is not None:
                _uuid = int(monster_data.get('uuid', 0) or 0)
                _target = int(getattr(self, '_bb_last_target_uuid', 0) or 0)
                if _uuid and _target and _uuid == _target:
                    if monster_data.get('is_dead', False):
                        ov.clear_target()
                    else:
                        raw_buffs = monster_data.get('buff_list', None) or []
                        # bridge 不走 monster 路, 这里现场 resolve buff 名字
                        from packet_bridge import _get_skill_name as _bm_name
                        packed = []
                        for b in raw_buffs:
                            if not isinstance(b, dict):
                                continue
                            bid = int(b.get('buff_id', 0) or 0)
                            if bid <= 0:
                                continue
                            packed.append({
                                'id': bid,
                                'uuid': int(b.get('buff_uuid', 0) or 0),
                                'begin_ms': int(b.get('begin_time', 0) or 0),
                                'duration_ms': int(b.get('duration', 0) or 0),
                                'layer': int(b.get('layer', 0) or 0),
                                'count': int(b.get('count', 0) or 0),
                                'name': _bm_name(bid),
                            })
                        # 把 server offset 一起带过去
                        try:
                            offset = float(getattr(self._state_mgr.state,
                                                   'server_time_offset_ms', 0.0) or 0.0)
                        except Exception:
                            offset = 0.0
                        ov.update_target(
                            _uuid, monster_data.get('name', ''), packed, offset)
                        # First-time debug print: 第一次成功推 boss buff
                        if not getattr(self, '_dbg_buffmon_boss_first', False):
                            self._dbg_buffmon_boss_first = True
                            try:
                                from config import BUFFMON_DEBUG as _bm_dbg
                            except Exception:
                                _bm_dbg = False
                            if _bm_dbg:
                                print(f'[SAO Entity] BuffMon boss FIRST push: '
                                      f'target_uuid={_uuid} name={monster_data.get("name","")!r} '
                                      f'buffs={len(packed)}', flush=True)
        except Exception:
            pass

    def _on_boss_event(self, event):
        """Boss buff/event callback from packet_parser → boss raid engine."""
        if self._boss_raid_engine:
            try: self._boss_raid_engine.on_boss_event(event)
            except Exception: pass

    def _on_scene_change(self, scene_event=None):
        """Packet parser scene/retry callback: clear DPS/BossHP encounter state."""
        _scene_kind = ''
        _scene_reason = ''
        _preserve_combat = False
        _reset_on_next_damage = False
        if isinstance(scene_event, dict):
            _scene_kind = str(scene_event.get('kind') or '')
            _scene_reason = str(scene_event.get('reason') or '')
            _preserve_combat = bool(scene_event.get('preserve_combat', False))
            _reset_on_next_damage = bool(scene_event.get('reset_on_next_damage', False))
        if _reset_on_next_damage:
            self._arm_pending_combat_reset(scene_event)
            return
        if _preserve_combat:
            # Same-dungeon layer/map transitions are common during long fights.
            # Keep the live encounter; only force the next overlay tick to refresh.
            print(
                f'[SAO Entity] ↔ 同副本软切换({ _scene_kind or "transition" }/{_scene_reason}) '
                    f'— 保留 BossHP 与 DPS',
                flush=True,
            )
            self._last_boss_hp_push_sig = None
            self._scene_damage_grace_until = max(
                float(getattr(self, '_scene_damage_grace_until', 0.0) or 0.0),
                time.time() + 10.0,
            )
            try:
                if self._dps_tracker:
                    self._dps_tracker.invalidate_snapshot_cache()
            except Exception:
                pass
            return

        _scene_change_ts = time.time()
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        self._scene_damage_grace_until = _scene_change_ts + 15.0
        print('[SAO Entity] ⚡ 场景/同副本重开 — 重置 BossHP 与 DPS 追踪', flush=True)
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_recent_targets = {}
        self._bb_last_hp_motion_sig = None
        self._bb_last_hp_motion_ts = 0.0
        self._last_boss_hp_push_sig = None

        try:
            if self._state_mgr:
                self._state_mgr.update(
                    boss_breaking_stage=-1,
                    boss_extinction_pct=0.0,
                    boss_current_hp=0,
                    boss_total_hp=0,
                    boss_hp_source='none',
                    boss_hp_est_pct=1.0,
                    boss_shield_active=False,
                    boss_shield_pct=0.0,
                    boss_in_overdrive=False,
                    boss_invincible=False,
                )
        except Exception:
            pass

        if self._dps_tracker:
            try:
                self._dps_tracker.reset()
                print('[SAO Entity] DPS tracker reset on scene change', flush=True)
            except Exception:
                pass
        self._dps_visible = False
        self._dps_faded = False
        self._dps_mode = 'hidden'

        if self._boss_raid_engine:
            try:
                if getattr(self._boss_raid_engine, '_state', '') != 'running':
                    self._boss_raid_engine.reset()
            except Exception:
                pass

        # Hard scene reset clears encounter data, but keep overlay windows
        # ready for the first fresh damage packet. Destroying the DPS window
        # here races the post-transition rebuild path and can leave the panel
        # visually suppressed after repeated map switches.
        try:
            if self._boss_hp_overlay:
                self._boss_hp_overlay.update({'active': False})
        except Exception:
            pass
        try:
            if self._boss_buff_overlay:
                self._boss_buff_overlay.clear_target()
        except Exception:
            pass
        try:
            self._sync_dps_report_availability()
        except Exception:
            pass

        self._scene_hide_token = int(getattr(self, '_scene_hide_token', 0) or 0) + 1
        _hide_token = self._scene_hide_token

        def _hide_overlays():
            # Re-hide once more after settling. The deferred pass is for any
            # paint that managed to push between the immediate hide above and
            # the next overlay tick.
            if _hide_token != int(getattr(self, '_scene_hide_token', 0) or 0):
                return
            _has_new_self_damage = bool(self._bb_last_damage_ts > _scene_change_ts)
            _has_new_dps = False
            try:
                _has_new_dps = bool(self._dps_tracker and self._dps_tracker.has_recent_damage(1.0))
            except Exception:
                _has_new_dps = False
            try:
                if (self._boss_hp_overlay
                        and not _has_new_self_damage
                        and not bool(getattr(self, '_bb_recent_targets', None))):
                    self._boss_hp_overlay.update({'active': False})
            except Exception:
                pass
            # Do not call DpsOverlay.hide() from this deferred guard. A fresh
            # post-transition damage packet can rebuild the tracker before the
            # next overlay tick has flipped _dps_visible back to True; hiding
            # here races that first live update and can leave the DPS panel
            # visually suppressed after several map switches.
            try:
                self._sync_dps_report_availability()
            except Exception:
                pass

        try:
            self.root.after(120, _hide_overlays)
        except Exception:
            pass

    def _start_recognition(self):
        """启动游戏数据引擎 (抓包 + 纯识图 + AutoKey + BossRaid)."""
        try:
            from game_state import GameStateManager
            from config import SettingsManager as CfgSettings

            self._state_mgr = GameStateManager()
            cfg_settings = CfgSettings()
            self._cfg_settings_ref = cfg_settings

            # v2.1.18: 必须先 subscribe 再 load_cache, 否则 load_cache 内部对订阅者
            # 的初始通知会因为 _on_game_state_update 还没注册而被丢弃, 导致 entity 面板
            # 启动时显示不出从 webview 切过来时持久化的角色名/等级/HP 等.
            try:
                self._state_mgr.subscribe(self._on_game_state_update)
            except Exception:
                pass

            # 加载上次缓存的游戏状态 (立即显示)
            self._state_mgr.load_cache(cfg_settings)

            # Restore sound settings
            try:
                from sao_sound import set_sound_enabled, set_sound_volume
                _snd_on = cfg_settings.get('sound_enabled', True)
                _snd_vol = cfg_settings.get('sound_volume', 70)
                set_sound_enabled(bool(_snd_on) if _snd_on is not None else True)
                set_sound_volume(int(_snd_vol) if _snd_vol is not None else 70)
            except Exception:
                pass

            # 用缓存名替换默认 "Player"
            cached_name = self._state_mgr.state.player_name
            if cached_name:
                self._username = cached_name
                disp = cached_name
                if len(disp) > 10:
                    disp = disp[:9] + '…'
                self._hp_display_name = disp
                print(f'[SAO Entity] 从缓存加载角色名: {cached_name}')

            self._reconfigure_data_engines()

            # AutoKey Engine
            self._auto_key_engine = AutoKeyEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                extra_gate=lambda: bool(getattr(self, '_recognition_active', False)),
            )
            self._auto_key_engine.start()
            try:
                self._auto_key_engine.set_burst_actions(self._load_autokey_burst_actions())
            except Exception:
                pass

            # Boss Raid Engine + AutoKey Linkage
            self._boss_autokey_linkage = BossAutoKeyLinkage(
                self._cfg_settings_ref,
                send_key=self._send_linked_key,
                on_log=lambda msg: print(msg),
            )

            def _on_boss_alert_with_linkage(title, message):
                print(f'[SAO Entity] Boss Alert: {title} — {message}')
                if self._alert_overlay:
                    try: self._alert_overlay.show_alert(title, message)
                    except Exception: pass
                if self._boss_autokey_linkage:
                    try:
                        self._boss_autokey_linkage.on_boss_raid_alert(title, message)
                    except Exception:
                        pass

            self._boss_raid_engine = BossRaidEngine(
                self._state_mgr,
                self._cfg_settings_ref,
                on_alert=_on_boss_alert_with_linkage,
                on_sound=lambda name: play_sound(name),
            )

            # ── 初始化 ULW 覆盖层 ──
            # DPS panel mirrors webview: stays hidden on startup regardless of
            # the `dps_enabled` setting. Combat/report triggers bring it up.
            self._dps_enabled = bool(self._get_setting('dps_enabled', True))
            self._dps_visible = False
            try:
                self._dps_overlay = DpsOverlay(
                    self.root,
                    self._cfg_settings_ref,
                    request_live_snapshot=self._request_dps_live_snapshot,
                    show_last_report=self._request_dps_last_report,
                    reset_dps=self._reset_dps_tracker,
                    has_last_report=self._get_dps_last_report_available,
                    request_entity_detail=self._request_dps_entity_detail,
                    alert=self._show_entity_alert,
                )
                self._dps_overlay.set_report_available(
                    self._get_dps_last_report_available())
                print('[SAO Entity] DPS overlay initialized (hidden)')
            except Exception as e:
                print(f'[SAO Entity] DPS overlay init failed: {e}')
                self._dps_overlay = None

            try:
                self._boss_hp_overlay = BossHpOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Boss HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Boss HP overlay init failed: {e}')
                self._boss_hp_overlay = None

            self._hp_ov_visible = bool(self._get_setting('hp_ov_enabled', True))
            try:
                self._hp_overlay = HpOverlay(
                    self.root, self._cfg_settings_ref,
                    on_click=self._hp_overlay_on_click,
                    on_menu=self._hp_overlay_on_menu,
                )
                if self._hp_ov_visible:
                    self._hp_overlay.show()
                # Push cached game state to HP overlay immediately
                if self._state_mgr:
                    try:
                        _gs = self._state_mgr.state
                        if _gs.hp_max > 0:
                            _hp_lv = int(_gs.level_base or 1)
                            _hp_lv_extra = int(getattr(_gs, 'level_extra', 0) or 0)
                            if _hp_lv_extra > 0 and _hp_lv > 0:
                                _hp_lv_text = f'{_hp_lv}(+{_hp_lv_extra})'
                            else:
                                _hp_lv_text = str(_hp_lv)
                            self._hp_overlay.update_hp(
                                _gs.hp_current, _gs.hp_max, _hp_lv_text)
                        if _gs.stamina_max > 0:
                            self._hp_overlay.update_sta(
                                _gs.stamina_current, _gs.stamina_max)
                        self._hp_overlay.set_sta_offline(
                            self._should_show_sta_offline(_gs)
                        )
                        if _gs.player_name:
                            self._hp_overlay.set_player_info({
                                'name': _gs.player_name,
                                'profession': _gs.profession_name or '',
                                'uid': _gs.player_id or '',
                            })
                    except Exception:
                        pass
                print('[SAO Entity] Player HP overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Player HP overlay init failed: {e}')
                self._hp_overlay = None

            try:
                self._alert_overlay = AlertOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] Alert overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] Alert overlay init failed: {e}')
                self._alert_overlay = None

            try:
                self._skillfx_overlay = BurstReadyOverlay(self.root, self._cfg_settings_ref)
                print('[SAO Entity] SkillFX (Burst) overlay initialized')
            except Exception as e:
                print(f'[SAO Entity] SkillFX overlay init failed: {e}')
                self._skillfx_overlay = None

            try:
                _bm_on = bool(self._get_setting('buffmon_enabled', True))
                self._self_buff_overlay = SelfBuffOverlay(
                    self.root, self._cfg_settings_ref, hp_overlay=self._hp_overlay)
                self._self_buff_overlay.set_enabled(_bm_on)
                self._boss_buff_overlay = BossBuffOverlay(
                    self.root, self._cfg_settings_ref,
                    boss_hp_overlay=self._boss_hp_overlay)
                self._boss_buff_overlay.set_enabled(_bm_on)
                print('[SAO Entity] BuffMon overlays (self/boss) initialized')
            except Exception as e:
                print(f'[SAO Entity] BuffMon overlay init failed: {e}')
                self._self_buff_overlay = None
                self._boss_buff_overlay = None

            # 启动定时缓存保存 (每30秒)
            import threading as _thr
            _stop_evt = self._cache_loop_stop
            def _cache_loop():
                while not _stop_evt.is_set():
                    _stop_evt.wait(30)
                    if _stop_evt.is_set():
                        break
                    try:
                        self._persist_cached_identity_state(save_now=False)
                        self._state_mgr.save_cache(self._cfg_settings_ref)
                    except Exception:
                        pass
            _thr.Thread(target=_cache_loop, daemon=True, name='cache_saver').start()

        except Exception as e:
            print(f'[SAO Entity] Data engine failed: {e}')
            import traceback; traceback.print_exc()
            self._recognition_active = False

    # ────────────────────────────────────────────
    #  SkillFX / Burst Mode Ready helpers
    # ────────────────────────────────────────────

    def _get_skillfx_layout(self, gs=None):
        """Compute the screen-relative layout for the BurstReady overlay.

        Mirrors the one in sao_webview._get_skillfx_layout so the tk
        port's anchor + callout positioning matches the original webview.

        v2.4.33: hot-loop geometry math moved to ``_sao_cy_uihelpers``.
        Python only collects the gs/window_locator inputs.
        """
        if gs is None and getattr(self, '_state_mgr', None) is not None:
            gs = self._state_mgr.state
        client_rect = getattr(gs, 'window_rect', None) if gs else None
        if not client_rect:
            try:
                from window_locator import WindowLocator
                client_rect = WindowLocator().get_rect()
            except Exception:
                client_rect = None
        if not client_rect:
            return None
        client_left, client_top = int(client_rect[0]), int(client_rect[1])

        slot_rects: List[Dict[str, Any]] = []
        for slot in list(getattr(gs, 'skill_slots', []) or []) if gs else []:
            if not isinstance(slot, dict):
                continue
            rect = slot.get('rect') or {}
            try:
                sx = int(rect.get('x', 0)); sy = int(rect.get('y', 0))
                sw = int(rect.get('w', 0)); sh = int(rect.get('h', 0))
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slot_rects.append({
                'index': idx,
                'screen_rect': {'x': client_left + sx, 'y': client_top + sy,
                                'w': sw, 'h': sh},
            })
        fallback: List[Dict[str, Any]] = []
        if not slot_rects:
            for item in get_skill_slot_rects(client_rect):
                left, top, right, bottom = item['bbox']
                fallback.append({
                    'index': int(item['index']),
                    'screen_rect': {'x': left, 'y': top,
                                    'w': right - left, 'h': bottom - top},
                })
        return _CY_UI.compute_skillfx_layout(client_rect, slot_rects, fallback)

    def _get_game_window_rect(self):
        rect = None
        try:
            gs = self._state_mgr.state if getattr(self, '_state_mgr', None) is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from window_locator import WindowLocator
            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()
                self._locator = locator
            result = locator.find_game_window()
            if result:
                _hwnd, _title, found_rect = result
                return tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return rect

    def _get_game_window_context(self):
        rect = None
        hwnd = 0
        try:
            gs = self._state_mgr.state if getattr(self, '_state_mgr', None) is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from window_locator import WindowLocator
            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()
                self._locator = locator
            result = locator.find_game_window()
            if result:
                hwnd, _title, found_rect = result
                return int(hwnd or 0), tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return int(hwnd or 0), rect

    def _format_level_text(self, level_base: int, level_extra: int) -> str:
        return _CY_UI.format_level_text(level_base, level_extra, self._level or 1)

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

    def _persist_cached_identity_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        cache = dict(settings.get('game_cache', {}) or {})
        name = str(getattr(self, '_username', '') or '').strip()
        profession = str(getattr(self, '_profession', '') or '').strip()
        level_base = int(getattr(self, '_level', 0) or 0)
        level_extra = int(getattr(self, '_level_extra', 0) or 0)
        season_exp = int(getattr(self, '_season_exp', 0) or 0)
        if name:
            cache['player_name'] = name
        if profession:
            cache['profession_name'] = profession
        if level_base > 0:
            cache['level_base'] = level_base
        if level_extra > 0:
            cache['level_extra'] = level_extra
        if season_exp > 0:
            cache['season_exp'] = season_exp
        gs = getattr(self, '_game_state', None)
        uid = str(getattr(gs, 'player_id', '') or '').strip() if gs is not None else ''
        if uid:
            cache['player_id'] = uid
        fight_point = int(getattr(gs, 'fight_point', 0) or 0) if gs is not None else 0
        if fight_point > 0:
            cache['fight_point'] = fight_point
        settings.set('game_cache', cache)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

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

    def _float_click(self, e):
        self._drag['x'] = e.x_root
        self._drag['y'] = e.y_root
        self._drag['dragging'] = False
        self._stop_float_breath()

    def _float_drag(self, e):
        dx = abs(e.x_root - self._drag['x'])
        dy = abs(e.y_root - self._drag['y'])
        if dx > 5 or dy > 5:
            self._drag['dragging'] = True
        if self._drag['dragging']:
            mx = e.x_root - self._fw // 2
            my = e.y_root - self._fh // 2
            self._float.geometry(f'+{mx}+{my}')

    def _float_release(self, e):
        if self._skip_canvas_click:
            self._skip_canvas_click = False
            return
        if self._drag['dragging']:
            try:
                self._breath_base_x = self._float.winfo_x()
                self._breath_base_y = self._float.winfo_y()
                self.settings.set('float_x', self._breath_base_x)
                self.settings.set('float_y', self._breath_base_y)
                self.settings.save()
            except Exception:
                pass
            self._breath_t0 = time.time()
            self._breath_active = True
            self._breath_step()
        else:
            self._toggle_sao_menu()

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

    def _raise_panel_window(self, panel):
        """把面板提到最前并取焦, 防止被 SAO overlay 或其他 topmost 挡住."""
        if panel is None:
            return
        win = getattr(panel, '_win', None)
        if win is None:
            return
        try:
            if not win.winfo_exists():
                return
            win.attributes('-topmost', True)
            win.lift()
            win.focus_force()
        except Exception:
            pass

    def _fade_panel_in(self, panel, target=0.92, duration_ms=350):
        """浮动面板淡入 — 平滑 ease-out 动画, 并确保鱼眼叠加层运行"""
        # 面板打开时, 如果鱼眼尚未启动则启动
        if self._fisheye_ov is None:
            try:
                self.root.after(100, self._start_fisheye_overlay)
            except Exception:
                pass

        t0 = time.time()
        dur = duration_ms / 1000.0

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / dur)
            et = 1 - (1 - t) ** 3  # ease_out
            panel.attributes('-alpha', target * et)
            if t < 1.0:
                try:
                    self.root.after(16, _step)
                except Exception:
                    pass

        _step()

    def _fade_panel_out(self, panel, attr_name, settings_key, duration_ms=220):
        """浮动面板淡出 — ease-in 动画, 完成后 destroy"""
        try: play_sound('alert_close')
        except: pass
        try:
            start_alpha = float(panel.attributes('-alpha'))
        except Exception:
            start_alpha = 0.92

        t0 = time.time()
        dur = duration_ms / 1000.0

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / dur)
            et = t * t  # ease_in
            panel.attributes('-alpha', start_alpha * (1.0 - et))
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
                setattr(self, attr_name, None)
                self.settings.set(settings_key, False)
                self.settings.save()
                self._maybe_stop_fisheye()

        _step()

    def _restore_panels(self):
        """恢复上次会话中打开的浮动面板"""
        # 如果面板处于隐藏状态则跳过恢复
        if self._panels_hidden:
            return
        if self.settings.get('show_status', False):
            if not (self._status_panel and self._status_panel.winfo_exists()):
                self._toggle_status_panel()

    # ──────────────────────────────────────────
    #  HP overlay click / context-menu hooks
    # ──────────────────────────────────────────

    def _hp_overlay_on_click(self):
        """Left-click on HP panel: open the SAO radial menu (web parity)."""
        try:
            self._toggle_sao_menu(allow_close=True)
        except Exception:
            pass

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
    def _play_link_start(self):
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        # 目标位置: 上次保存的位置, 否则右下角
        saved_x = self.settings.get('float_x', None)
        saved_y = self.settings.get('float_y', None)
        # 固定位置: 左下角覆盖整个底部区域 (统一 HUD)
        # 向右偏移 4% 屏宽以覆盖游戏原生 HP/STA 条
        _offset_pct = 0.04
        try:
            if hasattr(self, 'settings') and self.settings:
                _offset_pct = self.settings.get('hud_offset_x', 0.04)
        except Exception:
            pass
        _hp_x = int(sw * _offset_pct)
        _hp_y = sh - self._fh
        if saved_x is not None and saved_y is not None:
            fx_final = max(0, min(int(saved_x), sw - self._fw))
            fy_final = max(0, min(int(saved_y), sh - self._fh))
        else:
            fx_final = _hp_x
            fy_final = _hp_y
        # 起始位置: 屏幕正中央 (LinkStart 动画中心)
        fx_start = sw // 2 - self._fw // 2
        fy_start = sh // 2 + 80   # 略低于中心 (文字下方)

        try:
            from gpu_overlay_window import (
                suspend_gpu_overlay_creation as _suspend_gpu_overlays,
                resume_gpu_overlay_creation as _resume_gpu_overlays,
            )
        except Exception:
            _suspend_gpu_overlays = None  # type: ignore[assignment]
            _resume_gpu_overlays = None  # type: ignore[assignment]
        _gpu_overlays_suspended = False
        if _suspend_gpu_overlays is not None:
            try:
                _suspend_gpu_overlays()
                _gpu_overlays_suspended = True
            except Exception:
                _gpu_overlays_suspended = False

        def _resume_overlay_creation():
            nonlocal _gpu_overlays_suspended
            if not _gpu_overlays_suspended or _resume_gpu_overlays is None:
                return
            _gpu_overlays_suspended = False
            try:
                _resume_gpu_overlays()
            except Exception:
                pass

        def on_done():
            _resume_overlay_creation()
            self._float.geometry(f'{self._fw}x{self._fh}+{fx_start}+{fy_start}')
            self._float.deiconify()
            self._float.lift()
            self._play_motion_blur(closing=False)
            self._run_entry_animation(fx_start, fy_start, fx_final, fy_final)

        # Canvas 渲染 (SAO-UI 隧道模型)
        try:
            ls = SAOLinkStart(self.root, on_done=on_done)
            ls.play()
        except Exception:
            _resume_overlay_creation()
            raise

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
    def _setup_hotkeys(self):
        self._hotkey_mgr = SAOHotkeyManager(self.settings, {
            'toggle_recognition': lambda: self.root.after(0, self._toggle_recognition_menu),
            'toggle_topmost': lambda: self.root.after(0, self._toggle_topmost),
            'toggle_auto_script': lambda: self.root.after(0, self._toggle_auto_script),
            'hide_panels': lambda: self.root.after(0, self._toggle_hide_all_panels),
            'toggle_hide_seek': lambda: self.root.after(0, self._toggle_hide_seek),
            'boss_raid_start': lambda: self.root.after(0, self._toggle_boss_raid),
            'boss_raid_next_phase': lambda: self.root.after(0, self._boss_raid_next_phase),
        })

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

    def _set_setting(self, key: str, value):
        """Persist a setting to cfg_settings and save."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            self._cfg_settings_ref.set(key, value)
            try: self._cfg_settings_ref.save()
            except Exception: pass

    def _get_setting(self, key: str, default=None):
        """Read a setting."""
        if hasattr(self, '_cfg_settings_ref') and self._cfg_settings_ref:
            return self._cfg_settings_ref.get(key, default)
        return default

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

    def _switch_to_old_ui(self):
        """Old UI 已移除 — no-op"""
        pass

    def _show_about(self):
        if self._sao_menu is not None and self._sao_menu.visible:
            self._sao_menu.close()
        try:
            from sao_updater import get_manager, STATE_AVAILABLE, STATE_READY
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

    def _show_leaderboard(self):
        """排行榜已移除 — no-op"""
        pass

    def _cleanup_exit_overlay(self):
        ov = getattr(self, '_exit_overlay', None)
        if not ov:
            return
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

    def _init_entry_boot_gl(self, width, height):
        try:
            import moderngl
        except Exception:
            return None
        try:
            ctx = moderngl.create_standalone_context()
            prog = ctx.program(
                vertex_shader='''
#version 330
out vec2 uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
in vec2 uv;
uniform vec2 u_resolution;
uniform float u_progress;
out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float band(float x, float center, float width) {
    return exp(-pow((x - center) / max(0.0001, width), 2.0));
}

void main() {
    vec2 p = uv - 0.5;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(p);
    float progress = clamp(u_progress, 0.0, 1.0);

    float openA = smoothstep(0.00, 0.22, progress);
    float openB = smoothstep(0.12, 0.56, progress);
    float settle = smoothstep(0.40, 1.00, progress);
    // barrel distortion strongest at ignition, flattens as screen opens
    float barrelK = mix(0.44, 0.0, smoothstep(0.0, 0.34, progress));
    vec2 bv = uv - 0.5;
    vec2 distUv = uv + bv * barrelK * dot(bv, bv);

    float halfH = mix(0.003, 0.50, openA);
    float halfW = mix(0.030, 0.62, openB);
    float maskY = 1.0 - smoothstep(halfH, halfH + 0.030, abs(distUv.y - 0.5));
    float maskX = 1.0 - smoothstep(halfW, halfW + 0.045, abs(distUv.x - 0.5));
    float screenMask = clamp(maskX * maskY, 0.0, 1.0);

    // overexposure flash: floods full frame at the moment the screen fires on
    float overexpose = smoothstep(0.0, 0.06, progress) * (1.0 - smoothstep(0.14, 0.40, progress));

    float ignition = band(uv.y, 0.5, mix(0.0016, 0.020, openA)) * (1.0 - smoothstep(0.18, 0.46, progress));
    float flare = exp(-r * mix(24.0, 6.5, openB)) * (0.45 + 0.55 * (1.0 - settle));
    float scan = 0.92 + 0.08 * sin((uv.y * u_resolution.y) * 2.8 + progress * 1100.0);
    float noise = hash21(gl_FragCoord.xy * 0.03 + progress * 17.0) * 0.05;
    float bezel = smoothstep(0.90, 0.18, max(abs(p.x) * 0.92, abs(p.y) * 1.25));
    float sweep = band(uv.y, 0.26 + progress * 0.48, 0.045) + band(uv.y, 0.60 + progress * 0.12, 0.060) * 0.55;

    vec3 cyan = vec3(0.52, 0.92, 1.0);
    vec3 blue = vec3(0.08, 0.46, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 color = vec3(0.0);
    // full-frame overexposure bloom + cyan tint bleed at ignition
    color += white * overexpose * 2.60;
    color += vec3(0.70, 0.94, 1.0) * overexpose * exp(-r * 3.0) * 1.40;
    color += white * ignition * 1.6;
    color += mix(blue, cyan, 0.50) * flare * (0.65 + 0.35 * openB);
    color += cyan * screenMask * (0.18 + 0.24 * sweep + 0.18 * settle);
    color += white * screenMask * 0.10 * (1.0 - smoothstep(0.0, 0.4, r));
    color += vec3(0.78, 0.96, 1.0) * sweep * screenMask * 0.24;
    color *= scan * bezel;
    color += vec3(noise) * screenMask * 0.12;
    color = clamp(color, 0.0, 1.0);
    fragColor = vec4(color, 1.0);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 4)
            fbo = ctx.framebuffer(color_attachments=[tex])
            prog['u_resolution'].value = (float(width), float(height))
            return {
                'ctx': ctx,
                'boot_prog': prog,
                'boot_vao': vao,
                'boot_tex': tex,
                'boot_fbo': fbo,
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            return None

    def _draw_entry_boot_gl(self, cv, ov, progress):
        gl = ov.get('gl')
        if not gl:
            return False
        try:
            prog = gl['boot_prog']
            fbo = gl['boot_fbo']
            vao = gl['boot_vao']
            fbo.use()
            gl['ctx'].clear(0.0, 0.0, 0.0, 1.0)
            prog['u_progress'].value = float(max(0.0, min(1.0, progress)))
            vao.render()
            raw = fbo.read(components=4, alignment=1)
            _h, _w = ov['sh'], ov['sw']
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(_h, _w, 4)
            pil_img = ov.get('_gl_pil_buf')
            if pil_img is None or pil_img.size != (_w, _h):
                pil_img = Image.fromarray(arr[::-1], 'RGBA')
                ov['_gl_pil_buf'] = pil_img
                photo = ImageTk.PhotoImage(pil_img)
                ov['gl_photo'] = photo
                ov['_gl_canvas_id'] = cv.create_image(0, 0, image=photo, anchor='nw')
            else:
                pil_img.frombytes(arr[::-1].tobytes())
                photo = ov['gl_photo']
                photo.paste(pil_img)
                # canvas item already exists — just update reference
            return True
        except Exception:
            return False

    def _create_entry_overlay(self, start_x, start_y, end_x, end_y):
        self._cleanup_entry_overlay()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        ov = tk.Toplevel(self.root)
        ov.overrideredirect(True)
        ov.attributes('-topmost', True)
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#060a10')
        ov.attributes('-alpha', 0.0)
        try:
            _disable_native_window_shadow(ov)
        except Exception:
            pass
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#060a10', highlightthickness=0, bd=0)
        cv.pack(fill=tk.BOTH, expand=True)
        self._entry_overlay = {
            'win': ov,
            'cv': cv,
            'sw': sw,
            'sh': sh,
            'gl': self._init_entry_boot_gl(sw, sh),
            'gl_photo': None,
            'start_x': start_x + self._fw // 2,
            'start_y': start_y + self._fh // 2,
            'end_x': end_x + self._fw // 2,
            'end_y': end_y + self._fh // 2,
        }
        return self._entry_overlay

    def _draw_entry_overlay(self, progress):
        ov = getattr(self, '_entry_overlay', None)
        if not ov:
            return
        try:
            win = ov['win']
            cv = ov['cv']
            if not win.winfo_exists() or not cv.winfo_exists():
                return
        except Exception:
            return

        sw, sh = ov['sw'], ov['sh']
        ignite_t = min(1.0, progress / 0.30)
        deploy_t = max(0.0, min(1.0, (progress - 0.10) / 0.70))
        settle_t = max(0.0, min(1.0, (progress - 0.80) / 0.20))
        bloom = ease_out(ignite_t)
        deploy = ease_in_out(deploy_t)
        settle = ease_in_out(settle_t)
        cx = int(lerp(ov['start_x'], ov['end_x'], deploy))
        cy = int(lerp(ov['start_y'], ov['end_y'], deploy))
        cyan = '#86dfff'
        gold = '#f3af12'
        white = '#edf7ff'
        dim_cyan = '#173746'
        dim_gold = '#5e4211'
        if progress <= 0.68:
            boot_t = progress / 0.68
        else:
            boot_t = 1.0

        try:
            if progress < 0.14:
                overlay_alpha = lerp(0.18, 0.94, ease_out(progress / 0.14))
            elif progress < 0.80:
                overlay_alpha = lerp(0.94, 0.74, ease_in_out((progress - 0.14) / 0.66))
            else:
                overlay_alpha = 0.74 * (1.0 - settle)
            win.attributes('-alpha', max(0.0, min(0.94, overlay_alpha)))
        except Exception:
            pass

        cv.delete('all')
        boot_gl_drawn = self._draw_entry_boot_gl(cv, ov, boot_t)
        if not boot_gl_drawn:
            scan_pitch = 24
            scan_shift = int((progress * 240) % scan_pitch)
            for y in range(-scan_pitch, sh + scan_pitch, scan_pitch):
                yy = y + scan_shift
                col = dim_cyan if ((y // scan_pitch) % 2 == 0) else '#101823'
                cv.create_line(0, yy, sw, yy, fill=col, width=1)

        if settle > 0.82:
            return

        span = int(lerp(min(sw * 0.42, 520), min(sw * 0.22, 260), deploy))
        aperture = int(lerp(172, 28, deploy))
        for off, col in [(-54, cyan), (-24, dim_cyan), (24, dim_gold), (54, gold)]:
            cv.create_line(cx - span, cy + off, cx - aperture, cy + off, fill=col, width=1)
            cv.create_line(cx + aperture, cy + off, cx + span, cy + off, fill=col, width=1)

        ring_r = int(lerp(220, 64, deploy))
        for extra, col in [(0, cyan), (24, gold)]:
            r = ring_r + extra
            arm = 22 + extra // 4
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = cx + sx * r
                    py = cy + sy * r
                    cv.create_line(px, py, px - sx * arm, py, fill=col, width=1)
                    cv.create_line(px, py, px, py - sy * arm, fill=col, width=1)

        diamond = int(lerp(28, 10, deploy))
        cv.create_polygon(cx, cy - diamond, cx + diamond, cy,
                          cx, cy + diamond, cx - diamond, cy,
                          outline=white, fill='')
        cv.create_line(cx - 46, cy, cx + 46, cy, fill=white, width=1)
        cv.create_line(cx, cy - 22, cx, cy + 22, fill=white, width=1)

        if not boot_gl_drawn or progress > 0.28:
            pulse_y = int(lerp(cy - 160, cy + 88, bloom))
            cv.create_line(max(0, cx - span - 150), pulse_y,
                           min(sw, cx + span + 150), pulse_y,
                           fill=cyan, width=1)
            cv.create_line(max(0, cx - span - 110), pulse_y + 3,
                           min(sw, cx + span + 110), pulse_y + 3,
                           fill=dim_cyan, width=1)

        label_x1 = max(30, cx - span - 70)
        label_x2 = min(sw - 30, cx + span + 70)
        cv.create_text(label_x1, max(24, cy - 164), text='SYS:ENTITY',
                       anchor='w', fill=cyan, font=('Consolas', 9))
        cv.create_text(label_x2, max(24, cy - 164), text='SEQ:ENTRY',
                       anchor='e', fill=gold, font=('Consolas', 9))
        cv.create_text(label_x1, min(sh - 24, cy + 174), text='STATUS:DEPLOY',
                       anchor='w', fill=dim_cyan, font=('Consolas', 9))
        cv.create_text(label_x2, min(sh - 24, cy + 174), text=time.strftime('%H:%M:%S'),
                       anchor='e', fill=dim_gold, font=('Consolas', 9))

        text_y = cy + 92
        cv.create_text(cx, text_y, text='LINK START', fill=white,
                       font=get_sao_font(16, True))
        cv.create_text(cx, text_y + 26, text='ENTITY DEPLOYMENT', fill=gold,
                       font=('Consolas', 11, 'bold'))
        cv.create_text(cx, text_y + 48, text='INITIALIZING VISUAL SHELL',
                       fill='#8aaec0', font=('Consolas', 9))

    def _run_entry_animation(self, fx_start, fy_start, fx_final, fy_final):
        self._create_entry_overlay(fx_start, fy_start, fx_final, fy_final)
        anim_start = time.time()
        total = 1.16
        phase1 = 0.34

        def _done():
            self._cleanup_entry_overlay()
            self._breath_base_x = fx_final
            self._breath_base_y = fy_final
            # self.root.after(120, self._start_float_breath)  # 禁用浮动
            self.root.after(160, self._animate_float_hud)
            # 启动识别循环
            self.root.after(200, self._start_recognition)
            self.root.after(600, self._recognition_loop)
            if not self._username:
                self.root.after(420, self._show_welcome_then_menu)
            else:
                self.root.after(420, self._toggle_sao_menu)
            self.root.after(900, self._restore_panels)
            self.root.after(220, self._mark_update_popup_ready)

        def _tick():
            if self._destroyed:
                self._cleanup_entry_overlay()
                return
            try:
                if not self._float.winfo_exists():
                    self._cleanup_entry_overlay()
                    return
            except Exception:
                self._cleanup_entry_overlay()
                return

            elapsed = time.time() - anim_start
            t = min(1.0, elapsed / total)
            self._draw_entry_overlay(t)

            if elapsed < phase1:
                hold = ease_out(elapsed / phase1)
                self._set_float_alpha(0.12 * hold)
                try:
                    self.root.after(16, _tick)
                except Exception:
                    self._cleanup_entry_overlay()
                return

            deploy = min(1.0, (elapsed - phase1) / max(0.001, total - phase1))
            deploy_e = ease_out(deploy)
            fx = int(lerp(fx_start, fx_final, deploy_e))
            fy = int(lerp(fy_start, fy_final, deploy_e))
            self._float.geometry(f'+{fx}+{fy}')
            self._set_float_alpha(0.95 * ease_in_out(deploy))

            if elapsed < total:
                try:
                    self.root.after(16, _tick)
                except Exception:
                    self._cleanup_entry_overlay()
            else:
                self._float.geometry(f'+{fx_final}+{fy_final}')
                self._set_float_alpha(0.95)
                _done()

        _tick()

    def _get_exit_banner(self, mode='exit', target_label=None):
        if mode == 'switch':
            return {
                'primary': 'INTERFACE SHIFT',
                'secondary': (target_label or 'NEXT UI').upper(),
                'tertiary': 'TRANSFERRING CONTROL TO NEXT LAYER',
                'accent': '#f3af12',
                'accent_dim': '#5e4211',
            }
        return {
            'primary': 'SYSTEM LOG OUT',
            'secondary': 'SAO ENTITY',
            'tertiary': 'PERSISTING SESSION STATE',
            'accent': '#86dfff',
            'accent_dim': '#173746',
        }

    def _init_exit_pulse_gl(self, width, height):
        try:
            import moderngl
        except Exception:
            return None
        try:
            ctx = moderngl.create_standalone_context()
            prog = ctx.program(
                vertex_shader='''
#version 330
out vec2 uv;
vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 p = pos[gl_VertexID];
    uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
''',
                fragment_shader='''
#version 330
in vec2 uv;
uniform vec2 u_resolution;
uniform vec2 u_center;
uniform float u_progress;
out vec4 fragColor;

float band(float x, float center, float width) {
    return exp(-pow((x - center) / max(0.0001, width), 2.0));
}

float gridLine(vec2 q, vec2 dir, float scale, float width) {
    float v = abs(fract(dot(q, dir) * scale) - 0.5);
    return 1.0 - smoothstep(width, width + 0.018, v);
}

void main() {
    vec2 center = u_center / u_resolution;
    vec2 p = uv - center;
    p.x *= u_resolution.x / max(1.0, u_resolution.y);
    float r = length(p);
    float ang = atan(p.y, p.x);
    float progress = clamp(u_progress, 0.0, 1.0);
    vec2 dirA = normalize(vec2(1.0, 0.0));
    vec2 dirB = normalize(vec2(0.5, 0.8660254));
    vec2 dirC = normalize(vec2(-0.5, 0.8660254));
    float hexScale = mix(20.0, 34.0, smoothstep(0.14, 0.86, progress));
    float lineA = gridLine(p, dirA, hexScale, 0.030);
    float lineB = gridLine(p, dirB, hexScale, 0.028);
    float lineC = gridLine(p, dirC, hexScale, 0.028);
    float ringRadius = mix(0.02, 0.92, smoothstep(0.0, 0.80, progress));
    float ringWidth = mix(0.090, 0.014, progress);
    float ring = band(r, ringRadius, ringWidth);
    float echoInner = band(r, max(0.0, ringRadius - 0.085), ringWidth * 2.2) * (1.0 - smoothstep(0.26, 0.84, progress));
    float echoOuter = band(r, ringRadius + 0.055, ringWidth * 1.5) * (1.0 - smoothstep(0.48, 0.94, progress));
    float hexMask = band(r, ringRadius - 0.018, ringWidth * 3.8);
    float grid = max(lineA, max(lineB, lineC)) * hexMask;
    float arc = smoothstep(0.18, 1.0, 0.5 + 0.5 * sin(ang * 12.0 - progress * 18.0));
    float core = exp(-r * mix(58.0, 22.0, progress));
    float halo = exp(-r * 3.4) * smoothstep(0.00, 0.18, progress) * (1.0 - smoothstep(0.62, 1.0, progress));
    float sweep = band(uv.y, 0.18 + progress * 0.60, 0.035) + band(uv.y, 0.58 - progress * 0.08, 0.055) * 0.55;
    float closeV = smoothstep(0.82, 1.0, progress);
    float closeH = mix(0.50, 0.006, closeV);
    float closeW = mix(0.50, 0.018, closeV);
    float tvMaskY = 1.0 - smoothstep(closeH, closeH + 0.02, abs(uv.y - 0.5));
    float tvMaskX = 1.0 - smoothstep(closeW, closeW + 0.02, abs(uv.x - 0.5));
    float tvMask = mix(1.0, tvMaskY, smoothstep(0.82, 0.94, progress));
    tvMask *= mix(1.0, tvMaskX, smoothstep(0.92, 1.0, progress));
    float scanlines = 0.92 + 0.08 * sin((uv.y * u_resolution.y) * 2.4 + progress * 1500.0);
    float vignette = smoothstep(1.34, 0.28, r);

    vec3 cyan = vec3(0.60, 0.95, 1.0);
    vec3 blue = vec3(0.06, 0.48, 1.0);
    vec3 white = vec3(1.0, 1.0, 1.0);
    vec3 gold = vec3(1.0, 0.80, 0.28);
    vec3 color = vec3(0.0);

    color += mix(blue, cyan, 0.42) * ring * (0.58 + 0.42 * arc);
    color += cyan * echoInner * 0.82;
    color += gold * echoOuter * 0.44;
    color += mix(cyan, gold, 0.34) * grid * 0.58;
    color += white * core * (0.26 + 0.74 * (1.0 - smoothstep(0.16, 0.60, progress)));
    color += cyan * halo * 0.40;
    color += vec3(0.82, 0.96, 1.0) * sweep * (0.08 + ring * 0.24);
    color *= scanlines * vignette * tvMask;
    color = clamp(color, 0.0, 1.0);
    float alpha = clamp((ring * 0.88 + echoInner * 0.52 + grid * 0.30 + core * 0.82 + sweep * 0.16) * tvMask, 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
''')
            vao = ctx.vertex_array(prog, [])
            tex = ctx.texture((width, height), 4)
            fbo = ctx.framebuffer(color_attachments=[tex])
            prog['u_resolution'].value = (float(width), float(height))
            return {
                'ctx': ctx,
                'pulse_prog': prog,
                'pulse_vao': vao,
                'pulse_tex': tex,
                'pulse_fbo': fbo,
            }
        except Exception:
            try:
                ctx.release()
            except Exception:
                pass
            return None

    def _draw_exit_pulse_gl(self, cv, ov, cx, cy, purge_t):
        gl = ov.get('gl')
        if not gl:
            return False
        try:
            prog = gl['pulse_prog']
            fbo = gl['pulse_fbo']
            vao = gl['pulse_vao']
            fbo.use()
            gl['ctx'].clear(0.0, 0.0, 0.0, 0.0)
            prog['u_center'].value = (float(cx), float(cy))
            prog['u_progress'].value = float(max(0.0, min(1.0, purge_t)))
            vao.render()
            raw = fbo.read(components=4, alignment=1)
            _h, _w = ov['sh'], ov['sw']
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(_h, _w, 4)
            pil_img = ov.get('_gl_pil_buf')
            if pil_img is None or pil_img.size != (_w, _h):
                pil_img = Image.fromarray(arr[::-1], 'RGBA')
                ov['_gl_pil_buf'] = pil_img
                photo = ImageTk.PhotoImage(pil_img)
                ov['gl_photo'] = photo
                ov['_gl_canvas_id'] = cv.create_image(0, 0, image=photo, anchor='nw')
            else:
                pil_img.frombytes(arr[::-1].tobytes())
                photo = ov['gl_photo']
                photo.paste(pil_img)
            return True
        except Exception:
            return False

    def _create_exit_overlay(self, mode='exit', target_label=None):
        self._cleanup_exit_overlay()
        try:
            sw = self.root.winfo_screenwidth()
            sh = self.root.winfo_screenheight()
        except Exception:
            sw, sh = 1920, 1080
        try:
            ov = tk.Toplevel(self.root)
            ov.overrideredirect(True)
        except Exception:
            self._finalize_close()
            return None
        ov.attributes('-topmost', True)
        ov.geometry(f'{sw}x{sh}+0+0')
        ov.configure(bg='#060a10')
        ov.attributes('-alpha', 0.0)
        try:
            _disable_native_window_shadow(ov)
        except Exception:
            pass
        cv = tk.Canvas(ov, width=sw, height=sh, bg='#060a10', highlightthickness=0, bd=0)
        cv.pack(fill=tk.BOTH, expand=True)
        try:
            fx = self._float.winfo_rootx() + self._fw // 2
            fy = self._float.winfo_rooty() + self._fh // 2
        except Exception:
            fx, fy = sw // 2, sh // 2
        self._exit_overlay = {
            'win': ov,
            'cv': cv,
            'sw': sw,
            'sh': sh,
            'fx': fx,
            'fy': fy,
            'banner': self._get_exit_banner(mode, target_label),
            'mode': mode,
            'gl': self._init_exit_pulse_gl(sw, sh),
            'gl_photo': None,
        }
        return self._exit_overlay

    def _draw_exit_overlay(self, progress):
        ov = getattr(self, '_exit_overlay', None)
        if not ov:
            return
        try:
            win = ov['win']
            cv = ov['cv']
            if not win.winfo_exists() or not cv.winfo_exists():
                return
        except Exception:
            return

        sw, sh = ov['sw'], ov['sh']
        cx, cy = ov['fx'], ov['fy']
        lock_t = min(1.0, progress / 0.30)
        purge_t = max(0.0, min(1.0, (progress - 0.18) / 0.82))
        lock_e = ease_out(lock_t)
        purge_e = ease_in_out(purge_t)
        cyan = '#86dfff'
        gold = ov['banner']['accent']
        dim_cyan = '#173746'
        dim_gold = ov['banner']['accent_dim']
        white = '#edf7ff'

        wash = 0.22 + 0.78 * lock_e
        sweep = ((lock_t * 0.45) + purge_t * 1.2) % 1.0
        tv_fade = 0.0 if progress <= 0.84 else min(1.0, max(0.0, (progress - 0.84) / 0.16))
        tv_fade = ease_in_out(tv_fade)

        try:
            peak_alpha = min(0.96, 0.16 + 0.50 * lock_e + 0.18 * purge_e)
            win.attributes('-alpha', max(0.0, peak_alpha * (1.0 - tv_fade * 0.97)))
        except Exception:
            pass

        cv.delete('all')
        if purge_t < 0.06 or not ov.get('gl'):
            scan_pitch = 26
            scan_shift = int((progress * 280) % scan_pitch)
            for y in range(-scan_pitch, sh + scan_pitch, scan_pitch):
                yy = y + scan_shift
                col = dim_cyan if ((y // scan_pitch) % 2 == 0) else '#101823'
                cv.create_line(0, yy, sw, yy, fill=col, width=1)

        pulse_gl_drawn = False
        if purge_t > 0.0:
            pulse_gl_drawn = self._draw_exit_pulse_gl(cv, ov, cx, cy, purge_t)
            if not pulse_gl_drawn:
                pulse = max(0.0, 1.0 - abs(purge_t - 0.18) / 0.18)
                if pulse > 0.01:
                    if pulse > 0.72:
                        flash_fill = '#eefbff'
                        flash_stipple = 'gray25'
                    elif pulse > 0.38:
                        flash_fill = '#c8efff'
                        flash_stipple = 'gray25'
                    else:
                        flash_fill = '#8edfff'
                        flash_stipple = 'gray50'
                    cv.create_rectangle(0, 0, sw, sh, fill=flash_fill, outline='', stipple=flash_stipple)
                    bloom_r = int(lerp(40, min(sw, sh) * 0.32, pulse))
                    core_r = max(10, int(bloom_r * 0.26))
                    cv.create_oval(cx - bloom_r, cy - bloom_r,
                                   cx + bloom_r, cy + bloom_r,
                                   outline='#dff8ff', width=max(1, int(2 + pulse * 3)),
                                   stipple='gray25')
                    cv.create_oval(cx - core_r, cy - core_r,
                                   cx + core_r, cy + core_r,
                                   fill='#f8feff', outline='', stipple='gray25')

                if tv_fade > 0.0:
                    fade = int(255 * tv_fade)
                    fill = f'#{fade:02x}{fade:02x}{fade:02x}'
                    cv.create_rectangle(0, 0, sw, sh, fill=fill, outline='',
                                        stipple='gray50' if tv_fade < 0.7 else '')

        if tv_fade > 0.18:
            return

        span = int(lerp(min(sw * 0.30, 360), min(sw * 0.38, 460), lock_e))
        aperture = int(lerp(146, 22, purge_e))
        for off, col in [(-60, cyan), (-28, dim_cyan), (28, dim_gold), (60, gold)]:
            cv.create_line(cx - span, cy + off, cx - aperture, cy + off, fill=col, width=1)
            cv.create_line(cx + aperture, cy + off, cx + span, cy + off, fill=col, width=1)

        base_r = int(lerp(34, 194, lock_e * (1.0 - purge_e * 0.20)))
        for extra, col in [(0, cyan), (20, gold)]:
            r = max(22, int((base_r + extra) * (1.0 - 0.58 * purge_e)))
            arm = 18 + extra // 3
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = cx + sx * r
                    py = cy + sy * r
                    cv.create_line(px, py, px - sx * arm, py, fill=col, width=1)
                    cv.create_line(px, py, px, py - sy * arm, fill=col, width=1)

        diamond = int(lerp(24, 9, purge_e))
        cv.create_polygon(cx, cy - diamond, cx + diamond, cy,
                          cx, cy + diamond, cx - diamond, cy,
                          outline=white, fill='')
        cv.create_line(cx - 38, cy, cx + 38, cy, fill=white, width=1)
        cv.create_line(cx, cy - 18, cx, cy + 18, fill=white, width=1)

        if purge_t > 0.0 and not pulse_gl_drawn:
            burst = int(lerp(18, 220, purge_e))
            flash = '#d7f7ff' if purge_t < 0.7 else gold
            cv.create_line(cx - burst, cy, cx + burst, cy, fill=flash, width=2)
            cv.create_line(cx, cy - int(burst * 0.42), cx, cy + int(burst * 0.42), fill=flash, width=1)

        if tv_fade < 0.92:
            scan_y = int(lerp(cy - 140, cy + 120, sweep))
            cv.create_line(max(0, cx - span - 140), scan_y,
                           min(sw, cx + span + 140), scan_y,
                           fill=cyan, width=1)
            cv.create_line(max(0, cx - span - 120), scan_y + 3,
                           min(sw, cx + span + 120), scan_y + 3,
                           fill=dim_cyan, width=1)

        banner_x1 = max(30, cx - span - 60)
        banner_x2 = min(sw - 30, cx + span + 60)
        seq_label = 'SEQ:SHIFT' if ov.get('mode') == 'switch' else 'SEQ:EXIT'
        status_label = 'STATUS:LOCK' if purge_t < 0.08 else ('STATUS:TRANSFER' if ov.get('mode') == 'switch' else 'STATUS:PURGE')
        cv.create_text(banner_x1, max(24, cy - 150), text='SYS:ENTITY',
                       anchor='w', fill=cyan, font=('Consolas', 9))
        cv.create_text(banner_x2, max(24, cy - 150), text=seq_label,
                       anchor='e', fill=gold, font=('Consolas', 9))
        cv.create_text(banner_x1, min(sh - 24, cy + 164), text=status_label,
                       anchor='w', fill=dim_cyan, font=('Consolas', 9))
        cv.create_text(banner_x2, min(sh - 24, cy + 164), text=time.strftime('%H:%M:%S'),
                       anchor='e', fill=dim_gold, font=('Consolas', 9))

        text_y = cy + 86
        primary = 'ENTITY LOCK' if purge_t < 0.12 else ov['banner']['primary']
        tertiary = 'FREEZING UI STATE' if purge_t < 0.12 else ov['banner']['tertiary']
        cv.create_text(cx, text_y, text=primary,
                       fill=white, font=get_sao_font(16, True))
        cv.create_text(cx, text_y + 26, text=ov['banner']['secondary'],
                       fill=gold, font=('Consolas', 11, 'bold'))
        cv.create_text(cx, text_y + 48, text=tertiary,
                       fill='#8aaec0', font=('Consolas', 9))

    def _collect_exit_windows(self):
        wins = []
        seen = set()

        try:
            focus_x = self._float.winfo_x() + self._fw // 2
            focus_y = self._float.winfo_y() + self._fh // 2
        except Exception:
            focus_x = self.root.winfo_screenwidth() // 2
            focus_y = self.root.winfo_screenheight() // 2

        def _profile(x, y, role, order):
            dx = x - focus_x
            dy = y - focus_y
            dist = max(1.0, math.hypot(dx, dy))
            ux, uy = dx / dist, dy / dist
            if role == 'float':
                return {'delay': 0.28, 'duration': 0.52, 'travel': 86,
                        'ux': 1.0, 'uy': -0.25, 'movable': True}
            if role == 'panel':
                return {'delay': 0.12 + order * 0.085, 'duration': 0.40,
                        'travel': 48 + order * 12, 'ux': ux, 'uy': uy + 0.24, 'movable': True}
            if role == 'menu':
                return {'delay': 0.00, 'duration': 0.32, 'travel': 0,
                        'ux': 0.0, 'uy': 0.0, 'movable': False}
            if role == 'fisheye':
                return {'delay': 0.00, 'duration': 0.24, 'travel': 0,
                        'ux': 0.0, 'uy': 0.0, 'movable': False}
            return {'delay': 0.06, 'duration': 0.32, 'travel': 22,
                    'ux': ux, 'uy': uy, 'movable': True}

        def _add(win, role, order=0, ulw=False):
            if not win:
                return
            try:
                if not win.winfo_exists():
                    return
                wid = win.winfo_id()
                if wid in seen:
                    return
                seen.add(wid)
                try:
                    alpha = float(win.attributes('-alpha'))
                except Exception:
                    alpha = 1.0
                profile = _profile(win.winfo_x(), win.winfo_y(), role, order)
                wins.append({
                    'win': win,
                    'alpha': max(0.0, min(1.0, alpha)),
                    'x': win.winfo_x(),
                    'y': win.winfo_y(),
                    'role': role,
                    'ulw': ulw,
                    **profile,
                })
            except Exception:
                pass

        def _add_panel_owner(panel, order=0):
            if not panel:
                return
            try:
                if hasattr(panel, 'is_visible') and not panel.is_visible():
                    return
            except Exception:
                pass
            win = getattr(panel, '_win', None)
            if not win:
                return
            try:
                if hasattr(win, 'state') and str(win.state()) == 'withdrawn':
                    return
            except Exception:
                pass
            _add(win, 'panel', order=order)

        # HP float 使用 ULW，不能用 attributes('-alpha') 读写
        _float = getattr(self, '_float', None)
        if _float:
            try:
                if _float.winfo_exists():
                    wid = _float.winfo_id()
                    if wid not in seen:
                        seen.add(wid)
                        profile = _profile(_float.winfo_x(), _float.winfo_y(), 'float', 0)
                        wins.append({
                            'win': _float,
                            'alpha': getattr(self, '_float_alpha', 1.0),
                            'x': _float.winfo_x(),
                            'y': _float.winfo_y(),
                            'role': 'float',
                            'ulw': True,
                            **profile,
                        })
            except Exception:
                pass
        for idx, panel in enumerate([
            self._status_panel,
            getattr(self._autokey_panel, '_win', None),
            getattr(self._bossraid_panel, '_win', None),
            getattr(self._autokey_detail_panel, '_win', None),
            getattr(self._bossraid_detail_panel, '_win', None),
        ]):
            _add(panel, 'panel', order=idx)
        _add_panel_owner(self._commander_panel, order=5)
        _add(getattr(getattr(self, '_sao_menu', None), '_overlay', None), 'menu')
        _add(getattr(self, '_fisheye_ov', None), 'fisheye')
        # _hp_alpha_windows 已废弃 (ULW 内部渲染)
        return wins

    def _finalize_close(self):
        if self._close_finalized:
            return
        self._close_finalized = True
        self._destroyed = True
        self._breath_active = False
        self._lift_loop_active = False
        # ── Cancel all global after() IDs ──
        for _aid_attr in ('_panel_float_after_id', '_menu_refresh_after_id',
                          '_hide_seek_alert_after_id'):
            _aid = getattr(self, _aid_attr, None)
            if _aid is not None:
                try:
                    self.root.after_cancel(_aid)
                except Exception:
                    pass
                setattr(self, _aid_attr, None)
        # Cancel shared HUD fx tick (class-level)
        _fx_aid = getattr(SAOPlayerGUI, '_sao_fx_after_id', None)
        if _fx_aid is not None:
            try:
                self.root.after_cancel(_fx_aid)
            except Exception:
                pass
            SAOPlayerGUI._sao_fx_after_id = None
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
        self._cleanup_exit_overlay()
        if hasattr(self, '_hotkey_mgr'):
            self._hotkey_mgr.cleanup()
        try:
            if self._state_mgr:
                self._state_mgr.unsubscribe(self._on_game_state_update)
        except Exception:
            pass
        self._stop_fisheye_overlay()
        try:
            if self._sao_menu is not None:
                self._sao_menu.unbind_events()
                self._sao_menu.force_destroy_overlay()
        except Exception:
            pass
        # 停止识别引擎
        self._recognition_active = False
        self._cache_loop_stop.set()
        # Round 35: stop the boss-HP off-main worker before tearing down
        # overlays (it dereferences self._boss_hp_overlay indirectly via
        # the compute helper). Safe to call even if never started.
        try:
            self._stop_boss_hp_worker()
        except Exception:
            pass
        self._stop_recognition_engines()
        # 保存缓存
        if self._state_mgr and self._cfg_settings_ref:
            try:
                self._persist_entity_menu_state(save_now=False)
                self._persist_cached_identity_state(save_now=False)
                self._state_mgr.save_cache(self._cfg_settings_ref)
            except Exception:
                pass
        elif self._cfg_settings_ref:
            try:
                self._persist_entity_menu_state(save_now=False)
                self._persist_cached_identity_state(save_now=True)
            except Exception:
                pass
        # 销毁所有浮动面板
        for panel in [self._status_panel, self._update_panel]:
            try:
                if panel and panel.winfo_exists():
                    panel.destroy()
            except Exception:
                pass
        # 销毁 ULW 覆盖层 + 配置面板
        for ov in [self._dps_overlay, self._boss_hp_overlay, self._hp_overlay,
                   self._alert_overlay, self._skillfx_overlay,
                   self._self_buff_overlay, self._boss_buff_overlay]:
            try:
                if ov:
                    ov.destroy()
            except Exception:
                pass
        for pnl in [
            self._autokey_panel,
            self._bossraid_panel,
            self._autokey_detail_panel,
            self._bossraid_detail_panel,
            self._commander_panel,
        ]:
            try:
                if pnl:
                    pnl.destroy()
            except Exception:
                pass
        self._dps_overlay = None
        self._boss_hp_overlay = None
        self._hp_overlay = None
        self._alert_overlay = None
        self._skillfx_overlay = None
        self._self_buff_overlay = None
        self._boss_buff_overlay = None
        self._autokey_panel = None
        self._bossraid_panel = None
        self._autokey_detail_panel = None
        self._bossraid_detail_panel = None
        self._commander_panel = None
        self._destroy_hp_alpha_strip_windows()
        try:
            if self._float and self._float.winfo_exists():
                self._float.destroy()
        except Exception:
            pass
        try:
            self.root.quit()  # 退出 mainloop，由 run() 负责 destroy
        except Exception:
            pass

    def _run_exit_animation(self, after_shutdown=None, mode='exit', target_label=None):
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
        # ULW/GPU overlays (HP / BossHP / DPS) cannot be alpha-faded by
        # _collect_exit_windows() because their windows are layered. Ask
        # them to self-fade so the panels do not snap off-screen. DPS.hide()
        # tears down the GPU window immediately, so prefer fade_out() when
        # the overlay exposes it.
        for ov in (
            getattr(self, '_hp_overlay', None),
            getattr(self, '_boss_hp_overlay', None),
            getattr(self, '_dps_overlay', None),
        ):
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
        if not wins:
            self._draw_exit_overlay(1.0)
            self._finalize_close()
            if after_shutdown:
                try:
                    after_shutdown()
                except Exception:
                    pass
            return

        t0 = time.time()
        stage1 = 0.34
        stage2 = 0.82
        duration = stage1 + stage2

        def _finish():
            self._finalize_close()
            if after_shutdown:
                try:
                    after_shutdown()
                except Exception:
                    pass

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
                _finish()

        try:
            self.root.after(1, _step)
        except Exception:
            _finish()

    def _on_close(self):
        self._run_exit_animation(mode='exit', target_label='Desktop')

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()
        # mainloop 已退出 — 先停 GLFW pump 线程（v2.3.14 解耦后 pump 在独立线程
        # 上跑 GL；必须在 root.destroy() 之前 join，否则 daemon 线程会被强杀，
        # 留下未释放的 WGL 上下文。
        try:
            import gpu_overlay_window as _gow
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
