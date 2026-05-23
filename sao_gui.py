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
    # Round 59: 4 Win32 helpers moved out of sao_gui.py into sao_panel_ui.
    _get_icon_path, _apply_window_icon, _set_clickthrough_style,
    _disable_native_window_shadow,
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


def _set_process_app_id(app_id: str):
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
    except Exception:
        pass


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
from gui_modules.sao_gui_dialogs_mixin import SAOPlayerGUIDialogsMixin  # noqa: E402
from gui_modules.sao_gui_engine_lifecycle_mixin import SAOPlayerGUIEngineLifecycleMixin  # noqa: E402
from gui_modules.sao_gui_packet_callbacks_mixin import SAOPlayerGUIPacketCallbacksMixin  # noqa: E402
from gui_modules.sao_gui_float_hp_mixin import SAOPlayerGUIFloatHpMixin  # noqa: E402
from gui_modules.sao_gui_float_handlers_mixin import SAOPlayerGUIFloatHandlersMixin  # noqa: E402
from gui_modules.sao_gui_lifecycle_mixin import SAOPlayerGUILifecycleMixin  # noqa: E402
from gui_modules.sao_gui_panel_fx_mixin import SAOPlayerGUIPanelFxMixin  # noqa: E402
from gui_modules.sao_gui_link_animation_mixin import SAOPlayerGUILinkAnimationMixin  # noqa: E402


class SAOPlayerGUI(SAOPlayerGUIMenuMixin, SAOPlayerGUIFisheyeMixin, SAOPlayerGUIActionsMixin, SAOPlayerGUIEngineTogglesMixin, SAOPlayerGUIDpsThemeMixin, SAOPlayerGUIPanelsMixin, SAOPlayerGUIStatusUpdaterMixin, SAOPlayerGUIDialogsMixin, SAOPlayerGUIEngineLifecycleMixin, SAOPlayerGUIPacketCallbacksMixin, SAOPlayerGUIFloatHpMixin, SAOPlayerGUIFloatHandlersMixin, SAOPlayerGUILifecycleMixin, SAOPlayerGUIPanelFxMixin, SAOPlayerGUILinkAnimationMixin, SAOPlayerGUIStateMixin, SAOPlayerGUISessionMixin):
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

    def _render_hp_strip_image(self, *a, **kw):
        return None

    def _sync_hp_alpha_strip_windows(self):
        """(ULW 模式下不需要同步条带窗口)"""
        pass

    def _render_hp_shell(self, hover=False, scale=4):
        """(deprecated) HP 外壳已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _render_hp_dynamic(self):
        """(deprecated) HP 动态内容已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

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

    def _switch_to_old_ui(self):
        """Old UI 已移除 — no-op"""
        pass

    def _show_leaderboard(self):
        """排行榜已移除 — no-op"""
        pass

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
