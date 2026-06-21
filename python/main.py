# -*- coding: utf-8 -*-
"""
SAO Auto — 主程序入口

UI 模式:
  webview — SAO WebView UI (pywebview, 唯一模式)

额外模式:
  --test      单次截图测试识别
  --headless  无 HUD，仅终端输出

架构:
  sao_webview.py  — WebView 透明 HUD (pywebview + EdgeChromium)
    config.py       — 平台配置
    act_platform/   — 插件 SDK、运行时分发、渲染钩子
    plugins/        — 游戏适配器动态注入游戏逻辑
"""

import os
import sys
import time
import json
import argparse


# v2.1.3: 必须在任何 win32 / GUI 窗口创建之前设置 DPI 感知。
# - 开发模式下 python.exe 自带 PerMonitorV2 manifest, 无须显式设置;
# - PyInstaller bootloader (runw.exe) 默认 DPI-unaware → 高 DPI 屏上
#   GetClientRect 返回逻辑像素 (e.g. 1280x720) 而 PrintWindow 抓到的是
#   原生像素 (e.g. 1920x1080), 会让插件的屏幕坐标解析错位。
# 这里在 main.py 模块级 (sys.path bootstrap 之前) 立即调用,
# 同时 EXE manifest 也声明 PerMonitorV2 作为最早保险。
def _early_dpi_aware():
    try:
        import ctypes
        # SetProcessDpiAwarenessContext (Win 10 1703+) 最优, PerMonitorV2
        try:
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ctypes.c_void_p(-4)
            user32 = ctypes.windll.user32
            user32.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
            user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
            if user32.SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2):
                return
        except Exception:
            pass
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
            return
        except Exception:
            pass
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
    except Exception:
        pass


_early_dpi_aware()

# Nuitka: redirect all output to log file (no console window)
_is_compiled = not os.path.isfile(os.path.abspath(__file__))
if _is_compiled:
    sys.frozen = True
    _log_path = os.path.join(os.path.dirname(os.path.abspath(sys.executable)), '_nuitka.log')
    try:
        _log_f = open(_log_path, 'w', encoding='utf-8', buffering=1)
        sys.stdout = _log_f
        sys.stderr = _log_f
    except Exception:
        pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _bootstrap_runtime_overrides():
    """模块化 onedir 布局适配 + update.exe 提升 (bootstrap).

    在 PyInstaller onedir + noarchive=True + contents_directory='runtime' 下,
    sys.path 只包含 runtime/。但 build_release.bat 会把 proto/ assets/ web/
    icon.ico 提升到 EXE 顶层 (便于增量更新), 导致:
    - 插件运行时的协议/资源目录不在 sys.path → 插件导入失败
      - 开发时 sys.path 包含项目根, onefile 时 _MEIPASS 包含 proto/, 都正常
      - **只有 onedir 打包后会 ImportError**
    解决: 把 EXE 所在目录 (frozen) / 当前文件目录 (dev) 加入 sys.path 头部。

    同时:在最早时机调用 sao_updater.promote_runtime_update_exe(), 把
    runtime/update.exe 提升到顶层 (旧 update.exe 通过嵌套路径绕过 _collect_entries)。
    """
    try:
        _frozen = getattr(sys, 'frozen', False)
        if not _frozen:
            _frozen = 'python' not in os.path.basename(sys.executable).lower()
        if _frozen:
            exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        else:
            exe_dir = os.path.dirname(os.path.abspath(__file__))
        if exe_dir and exe_dir not in sys.path:
            sys.path.insert(0, exe_dir)
        # _MEIPASS 也兜底加入 (onefile 已默认在内, onedir 下 _MEIPASS = runtime/)
        meipass = getattr(sys, '_MEIPASS', None)
        if meipass and meipass not in sys.path:
            sys.path.insert(0, meipass)
        print(f'[main] bootstrap: frozen={getattr(sys, "frozen", False)} '
              f'exe_dir={exe_dir} meipass={meipass} '
              f'sys.path[0:3]={sys.path[0:3]}', flush=True)
    except Exception as e:
        print(f'[main] bootstrap path setup failed: {e}', flush=True)

    # config.py already owns all update.exe / update.exe.new bootstrap
    # promotion paths. Import it here to trigger those handlers exactly once
    # without re-running the older updater-side bootstrap.
    try:
        import config  # noqa: F401
    except Exception as e:
        print(f'[main] update.exe promote skipped: {e}', flush=True)
    return


_bootstrap_runtime_overrides()


def _set_dpi_aware():
    _early_dpi_aware()


def _dispatch_cli_runtime(action: str, missing_msg: str) -> None:
    """Dispatch a ``--test`` / ``--headless`` CLI action through the plugin runtime.

    The platform bootstraps the plugin manager (which triggers each plugin's
    ``on_load`` and therefore ``register_extension_runtime``) and then looks up
    the requested handler. The platform code never imports plugin modules.
    """
    try:
        from act_platform.runtime import (
            ensure_act_plugin_manager,
            _extension_runtime_handler,
        )
        owner = _CliRuntimeOwner()
        ensure_act_plugin_manager(owner, load=True)
    except Exception as exc:
        print(f'[SAO Auto] plugin manager bootstrap failed: {exc}')
        return
    handler = _extension_runtime_handler(action)
    if not callable(handler):
        print(missing_msg)
        return
    try:
        handler()
    except Exception as exc:
        print(f'[SAO Auto] {action} failed: {exc}')


class _CliRuntimeOwner:
    """Minimal owner for plugin-manager-driven CLI dispatch.

    ``main.py --test`` / ``--headless`` run before the full UI is online. We
    bootstrap only the pieces the plugin manager needs (settings + an attrs
    sink); the plugin's ``on_load`` then registers its runtime handlers via
    ``register_extension_runtime``.
    """

    def __init__(self) -> None:
        try:
            from config import SettingsManager
            self._cfg_settings_ref = SettingsManager()
        except Exception:
            self._cfg_settings_ref = None


def run_test():
    """Run the active plugin's one-shot CLI test handler."""
    _dispatch_cli_runtime(
        "cli_test",
        "[SAO Auto] --test requires an active plugin runtime handler.",
    )


def run_headless():
    """Run the active plugin's headless CLI handler."""
    _dispatch_cli_runtime(
        "cli_headless",
        "[SAO Auto] --headless requires an active plugin runtime handler.",
    )


def _start_update_check():
    """在 UI 启动后后台检查一次更新；状态由 sao_updater 管理器维护，UI 会自行监听。"""
    try:
        from config import SettingsManager
        s = SettingsManager()
        if not s.get('update_check_enabled', True):
            return
    except Exception:
        pass
    try:
        from updater.sao_updater import get_manager
        get_manager().check_async()
    except Exception as e:
        print(f'[SAO Auto] update check skipped: {e}')


def _register_apply_on_exit():
    """注册 atexit hook：如果退出时有 staging 待应用包，就启动外部 helper 应用它。"""
    import atexit
    def _hook():
        try:
            from updater.sao_updater import has_pending_update, schedule_apply_on_exit
            if has_pending_update():
                schedule_apply_on_exit()
        except Exception:
            pass
    atexit.register(_hook)


def _show_tk_license_gate():
    """启动前显示 Tk 授权验证弹窗 (阻塞到用户关闭)"""
    try:
        from license import get_license_manager
        mgr = get_license_manager()
        if mgr.is_paid:
            print('[license] paid license detected, skipping dialog')
            return
    except Exception as e:
        print(f'[license] check failed: {e}')
        return

    try:
        from gui_modules.sao_gui_license import is_license_dialog_dismissed
        if is_license_dialog_dismissed():
            print('[license] dialog previously dismissed, skipping')
            return
    except Exception:
        pass

    try:
        from gui_modules.sao_gui_license import show_license_dialog
        show_license_dialog()
    except Exception as e:
        print(f'[license] dialog failed: {e}')


def run_ui():
    """根据 settings.json 中的 ui_mode 启动对应 UI."""
    # ── Phase 0: 授权验证 (阻塞, 在 LinkStart / 引擎加载之前) ──
    _show_tk_license_gate()

    # 读取 ui_mode 设置
    ui_mode = 'entity'  # default: Entity/Tk
    try:
        from config import SettingsManager
        _s = SettingsManager()
        ui_mode = _s.get('ui_mode', 'entity') or 'entity'
        if ui_mode == 'sao':
            ui_mode = 'entity'
    except Exception:
        pass

    if ui_mode == 'entity':
        print('[SAO Auto] UI mode: entity (tkinter)')
        try:
            from sao_gui import SAOPlayerGUI
            app = SAOPlayerGUI()
            app.run()
            return
        except Exception as e:
            print(f'[SAO Auto] Entity UI 启动失败: {e}')
            import traceback; traceback.print_exc()
            # fall through to webview

    print('[SAO Auto] UI mode: webview')
    try:
        from sao_webview import SAOWebViewGUI, is_webview_available
        if is_webview_available():
            app = SAOWebViewGUI()
            app.run()
            return
        else:
            print('[SAO Auto] pywebview 不可用, 尝试 entity 模式')
    except Exception as e:
        print(f'[SAO Auto] WebView UI 启动失败: {e}')

    # Final fallback: entity mode
    if ui_mode != 'entity':
        try:
            from sao_gui import SAOPlayerGUI
            app = SAOPlayerGUI()
            app.run()
            return
        except Exception as e2:
            print(f'[SAO Auto] Entity UI 也启动失败: {e2}')

    print('[SAO Auto] 回退到 headless 模式')
    run_headless()


def _elevate_process_priority():
    """v2.1.16: nudge process to ABOVE_NORMAL on Windows.

    Helps the Tk main loop + render lanes keep timeslices when many panels
    are active and recognition/packet threads compete for CPU. ABOVE_NORMAL
    is conservative — it doesn't starve background apps the way HIGH would.
    """
    try:
        import ctypes
        ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)
    except Exception:
        pass


def main():
    _set_dpi_aware()
    _elevate_process_priority()
    _register_apply_on_exit()
    _start_update_check()

    parser = argparse.ArgumentParser(description='SAO Auto — 游戏 HUD 与自动化')
    parser.add_argument('--test', action='store_true', help='单次识别测试')
    parser.add_argument('--headless', action='store_true', help='无 HUD 终端模式')
    parser.add_argument('--ai-editor', action='store_true',
                        help='启动 AI Editor 独立窗口 (pywebview, 不进入主 UI)')
    parser.add_argument('--workshop', action='store_true',
                        help='启动创意工坊独立窗口 (pywebview, 不进入主 UI)')
    args = parser.parse_args()

    if args.ai_editor:
        from ai_editor.app import launch
        launch(blocking=True)
    elif args.workshop:
        from workshop.app import launch as ws_launch
        ws_launch(blocking=True)
    elif args.test:
        run_test()
    elif args.headless:
        run_headless()
    else:
        run_ui()


if __name__ == '__main__':
    main()
