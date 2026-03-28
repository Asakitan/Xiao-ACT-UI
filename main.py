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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


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


def run_ui():
    """根据 settings.json 中的 ui_mode 启动对应 UI."""
    # 读取 ui_mode 设置
    ui_mode = 'webview'  # default
    try:
        from config import SettingsManager
        _s = SettingsManager()
        ui_mode = _s.get('ui_mode', 'webview') or 'webview'
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

    parser = argparse.ArgumentParser(description='SAO Auto — 游戏 HUD 与自动化')
    parser.add_argument('--test', action='store_true', help='单次识别测试')
    parser.add_argument('--headless', action='store_true', help='无 HUD 终端模式')
    args = parser.parse_args()

    if args.test:
        run_test()
    elif args.headless:
        run_headless()
    else:
        run_ui()


if __name__ == '__main__':
    main()
