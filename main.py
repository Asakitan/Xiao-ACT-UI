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
  config.py       — 配置、ROI
  recognition.py  — 截图 + OCR + 像素条识别
  game_state.py   — 统一状态模型
  automation.py   — 自动化核心
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
#   原生像素 (e.g. 1920x1080) → STA 条裁剪坐标错位 → 颜色匹配 0 信号
#   → stamina_offline=True → HP 面板被 setSTAOffline(true) 隐藏。
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _bootstrap_runtime_overrides():
    """模块化 onedir 布局适配 + update.exe 提升 (bootstrap).

    在 PyInstaller onedir + noarchive=True + contents_directory='runtime' 下,
    sys.path 只包含 runtime/。但 build_release.bat 会把 proto/ assets/ web/
    icon.ico 提升到 EXE 顶层 (便于增量更新), 导致:
      - `from proto import star_resonance_pb2` 失败 → packet_parser 报错
      - 开发时 sys.path 包含项目根, onefile 时 _MEIPASS 包含 proto/, 都正常
      - **只有 onedir 打包后会 ImportError**
    解决: 把 EXE 所在目录 (frozen) / 当前文件目录 (dev) 加入 sys.path 头部。

    同时:在最早时机调用 sao_updater.promote_runtime_update_exe(), 把
    runtime/update.exe 提升到顶层 (旧 update.exe 通过嵌套路径绕过 _collect_entries)。
    """
    try:
        if getattr(sys, 'frozen', False):
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

    # update.exe bootstrap promotion: 把 runtime/update.exe 提升到顶层
    try:
        from sao_updater import promote_runtime_update_exe
        promoted = promote_runtime_update_exe()
        if promoted:
            print('[main] update.exe 已从 runtime/ 提升到顶层', flush=True)
    except Exception as e:
        print(f'[main] update.exe promote skipped: {e}', flush=True)
    return


_bootstrap_runtime_overrides()


def _set_dpi_aware():
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            import ctypes
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def run_test():
    """单次截图测试: 截取游戏窗口，执行一次识别，打印结果"""
    from config import SettingsManager
    from game_state import GameStateManager
    from recognition import RecognitionEngine

    print('=' * 50)
    print('  SAO Auto — 识别测试')
    print('=' * 50)

    settings = SettingsManager()
    state_mgr = GameStateManager()
    engine = RecognitionEngine(state_mgr, settings)

    print('\nScanning game window...')
    result = engine._locator.find_game_window()
    if result is None:
        print('Game window not found')
        print(f'   keywords: {engine._locator._keywords}')
        return

    hwnd, title, rect = result
    print(f'Window: {title}')
    print(f'  rect: {rect}')
    print(f'  size: {rect[2]-rect[0]}x{rect[3]-rect[1]}')

    print('\nRunning single capture...')
    data = engine.single_capture()
    if data:
        print('\nCapture result:')
        for k, v in data.items():
            print(f'  {k}: {v}')
    else:
        print('Capture failed')


def run_headless():
    """无 HUD 模式: 仅终端输出识别结果"""
    from config import SettingsManager
    from game_state import GameState, GameStateManager
    from automation import AutomationCore

    print('=' * 50)
    print('  SAO Auto — Headless 模式')
    print('  按 Ctrl+C 退出')
    print('=' * 50)

    settings = SettingsManager()
    state_mgr = GameStateManager()
    auto = AutomationCore(state_mgr, settings)

    def _on_state(state: GameState):
        if state.recognition_ok:
            print(f'\r[{state.level_text}] {state.player_name}  '
                  f'HP:{state.hp_text}({state.hp_pct:.0%})  '
                  f'体力:{state.stamina_text}({state.stamina_pct:.0%})  '
                  f'ID:{state.player_id}', end='', flush=True)
        else:
            print(f'\r⚠ {state.error_msg}', end='', flush=True)

    state_mgr.subscribe(_on_state)
    auto.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        auto.stop()
        print('\n\n已退出')


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
        from sao_updater import get_manager
        get_manager().check_async()
    except Exception as e:
        print(f'[SAO Auto] update check skipped: {e}')


def _register_apply_on_exit():
    """注册 atexit hook：如果退出时有 staging 待应用包，就启动外部 helper 应用它。"""
    import atexit
    def _hook():
        try:
            from sao_updater import has_pending_update, schedule_apply_on_exit
            if has_pending_update():
                schedule_apply_on_exit()
        except Exception:
            pass
    atexit.register(_hook)


def run_ui():
    """根据 settings.json 中的 ui_mode 启动对应 UI."""
    # 读取 ui_mode 设置
    ui_mode = 'webview'  # default
    try:
        from config import SettingsManager
        _s = SettingsManager()
        ui_mode = _s.get('ui_mode', 'webview') or 'webview'
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


def main():
    _set_dpi_aware()
    _register_apply_on_exit()

    parser = argparse.ArgumentParser(description='SAO Auto — 游戏 HUD 与自动化')
    parser.add_argument('--test', action='store_true', help='单次识别测试')
    parser.add_argument('--headless', action='store_true', help='无 HUD 终端模式')
    args = parser.parse_args()

    if args.test:
        run_test()
    elif args.headless:
        run_headless()
    else:
        _start_update_check()
        run_ui()


if __name__ == '__main__':
    main()
