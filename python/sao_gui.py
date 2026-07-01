# -*- coding: utf-8 -*-
"""
SAO Utils 风格完整 GUI — 独立 UI 壳

包含 SAO PopUpMenu 菜单系统, 通用提示对话框,
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
    resource_path,
)
from sao_theme import (
    SAOColors, SAOButton, SAOProgressBar, SAOTitleBar, SAODialog,
    SAOLeaderboardDialog,
    SAOStatusPill, SAOResizeGrip, SAOFilePicker, SAOSeparator,
    SAOPopUpMenu, SAOLinkStart, SAOCircleButton,
    Animator, lerp, lerp_color, ease_out, ease_in_out,
    _close_alert as _sao_close_dialog,
)
from utils.sao_sound import play_sound, LevelUpEffect, load_sao_fonts, get_sao_font, get_cjk_font
from gui_modules.sao_gui_plugin_manager import PluginManagerPanel
from gui_modules.sao_panel_ui import (
    _apply_panel_style, _hex_rgba, _make_panel_close_button,
    _sao_panel_header, _bind_panel_drag, _sao_panel_body,
    _sao_panel_hud_canvas, _sao_row, _sao_pill,
    _get_icon_path, _apply_window_icon, _set_clickthrough_style,
    _disable_native_window_shadow,
    _set_process_app_id,
    _SAO_PANEL_BG, _SAO_PANEL_HEADER_BG, _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_BORDER, _SAO_PANEL_ACCENT, _SAO_PANEL_GOLD,
    _SAO_PANEL_SEP, _SAO_PANEL_BODY_BG, _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_VALUE_FG,
)


try:
    import pynput.keyboard as pynput_kb
    from pynput.keyboard import Key, KeyCode
    PYNPUT_HOTKEY_AVAILABLE = True
except Exception:
    pynput_kb = None
    Key = None
    KeyCode = None
    PYNPUT_HOTKEY_AVAILABLE = False


CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'settings.json')

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


from gui_modules.settings_manager import SettingsManager  # noqa: E402

from gui_modules.sao_hotkey_manager import SAOHotkeyManager  # noqa: E402


# ══════════════════════════════════════════════════════════
#  SAO Player GUI — 纯悬浮 SAO Menu 架构
# ══════════════════════════════════════════════════════════
# SAOPlayerGUI is split across platform mixins. Plugin-owned behavior is
# injected at runtime through the plugin SDK.
from gui_modules.sao_gui_menu_mixin import SAOPlayerGUIMenuMixin  # noqa: E402
from gui_modules.sao_gui_fisheye_mixin import SAOPlayerGUIFisheyeMixin  # noqa: E402
from gui_modules.sao_gui_panels_mixin import SAOPlayerGUIPanelsMixin  # noqa: E402
from gui_modules.sao_gui_status_updater_mixin import SAOPlayerGUIStatusUpdaterMixin  # noqa: E402
from gui_modules.sao_gui_dialogs_mixin import SAOPlayerGUIDialogsMixin  # noqa: E402
# EngineLifecycleMixin moved to plugin
# Game mixins (PacketCallbacksMixin/DamageEventsMixin) moved to plugin in 5.0.0
from gui_modules.sao_gui_float_chrome_mixin import SAOPlayerGUIFloatChromeMixin  # noqa: E402
from gui_modules.sao_gui_float_handlers_mixin import SAOPlayerGUIFloatHandlersMixin  # noqa: E402
from gui_modules.sao_gui_lifecycle_mixin import SAOPlayerGUILifecycleMixin  # noqa: E402
from gui_modules.sao_gui_panel_fx_mixin import SAOPlayerGUIPanelFxMixin  # noqa: E402
from gui_modules.sao_gui_link_animation_mixin import SAOPlayerGUILinkAnimationMixin  # noqa: E402
# SAOPlayerGUIDamageEventsMixin → moved to plugin
from gui_modules.sao_gui_misc_mixin import SAOPlayerGUIMiscMixin  # noqa: E402


class SAOPlayerGUI(SAOPlayerGUIMenuMixin, SAOPlayerGUIFisheyeMixin, SAOPlayerGUIPanelsMixin, SAOPlayerGUIStatusUpdaterMixin, SAOPlayerGUIDialogsMixin, SAOPlayerGUIFloatChromeMixin, SAOPlayerGUIFloatHandlersMixin, SAOPlayerGUILifecycleMixin, SAOPlayerGUIPanelFxMixin, SAOPlayerGUILinkAnimationMixin, SAOPlayerGUIMiscMixin):
    """
    纯悬浮 SAO Utils 风格 GUI — 没有传统窗口！
    - 常驻: 小型悬浮触发按钮 (Toplevel)
    - 展开: SAO PopUpMenu 全屏菜单 = 主界面
    - 菜单按钮: 平台分类 + 插件动态分类
    - 子菜单: 实时工具与面板控制
    - 可选: 浮动钢琴/可视化面板
    """

    def __init__(self):
        _set_process_app_id('sao.auto.platform.ui')
        self.root = tk.Tk()
        self.root.withdraw()  # root 永远隐藏, 只作为 Tk 事件循环
        self.root.title("SAO Auto — Platform UI")

        # Unified overlay: single DWM window for all GPU panels.
        try:
            from config import USE_UNIFIED_OVERLAY
            if USE_UNIFIED_OVERLAY:
                from render.gpu_overlay_window import (
                    set_unified_overlay_mode, prestart_unified_overlay)
                set_unified_overlay_mode(True)
                prestart_unified_overlay(self.root)
        except Exception:
            pass

        self.settings = SettingsManager()
        # 记录当前 UI 模式 — 下次启动时使用
        self.settings.set('ui_mode', 'entity')
        self.settings.save()

        # 角色身份/赛季进度/体力字段由游戏插件 on_load 注入；平台不持有任何
        # 游戏字段定义 (插件通过 entity_menu_bridge.initialize_owner_state 设置)。

        self._current_file = None
        self._panels_hidden = False  # 一键隐藏所有面板
        self._hidden_panels_snapshot = []  # 隐藏前记录哪些面板是开的
        self._picker = None        # SAOFilePicker 引用 (防止 GC)
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
        self._fisheye_ov = None    # 菜单开启时的持久鱼眼叠加层
        self._fisheye_hit_layer = None
        self._ctx_menu_open = False  # 右键菜单弹出中, 暂停 z-order 置顶
        self._lift_loop_active = False
        self._skip_canvas_click = False
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

        # ── 识别引擎 ──
        self._recognition_active = False
        self._recognition_engine = None
        self._recognition_engines = []
        self._cfg_settings_ref = self.settings
        self._cache_loop_stop = threading.Event()
        self._recog_lock = threading.Lock()

        self._panel_float_entries = {}
        self._panel_float_after_id = None

        # ── 配置面板实例 ──
        self._act_plugin_manager_panel = None  # PluginManagerPanel
        self._ai_editor_panel = None  # AIEditorPanel
        self._act_plugin_lifecycle_token = ""

        self._sao_menu = None  # lazy-init on first _toggle_sao_menu()
        self._init_wnd_shield()
        self._set_icon()
        self._create_floating_widget()
        self._setup_hotkeys()
        self.root.after(0, self._ensure_plugin_lifecycle_subscription)
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

    def _init_wnd_shield(self):
        try:
            from mem_probe.rt_io import WndShield
            self._wnd_shield = WndShield()
        except Exception:
            self._wnd_shield = None

    def register_wnd_shield(self, hwnd: int):
        sh = getattr(self, '_wnd_shield', None)
        if sh and hwnd:
            sh.register(hwnd)

    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        def _ensure_compositor_poller():
            try:
                from render.gpu_overlay_window import _unified_overlay_instance
                uo = _unified_overlay_instance
                if uo is not None:
                    uo.ensure_tk_poller()
                else:
                    self.root.after(500, _ensure_compositor_poller)
            except Exception:
                pass
        try:
            self.root.after(100, _ensure_compositor_poller)
        except Exception:
            pass
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
