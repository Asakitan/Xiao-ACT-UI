# -*- coding: utf-8 -*-
# Star Resonance standalone CLI entry points.
#
# The platform's ``main.py`` (``--test`` / ``--headless`` flags) used to import
# this plugin's engines directly. In 5.0.0 that coupling was removed — these CLI
# entry points now live inside the plugin, and ``main.py`` dispatches to them via
# ``ensure_act_plugin_manager`` + this module's ``run_test`` / ``run_headless``
# function.
#
# The host discovers them by ``manager.runtime('cli_test')`` /
# ``manager.runtime('cli_headless')`` or directly via a plugin-provided hook;
# this module is intentionally importable by both dev and frozen PyInstaller
# trees (it lives under the plugin's package path).

from __future__ import annotations

import time


def _settings():
    from config import SettingsManager
    return SettingsManager()


def run_test() -> None:
    # 单次截图测试: 截取游戏窗口，执行一次识别，打印结果。
    from plugins.star_resonance_plugin.engines.game_state import GameStateManager
    from plugins.star_resonance_plugin.vision.recognition import RecognitionEngine

    print('=' * 50)
    print('  SAO Auto — 识别测试')
    print('=' * 50)

    settings = _settings()
    state_mgr = GameStateManager()
    engine = RecognitionEngine(state_mgr, settings)

    print('\nScanning game window...')
    result = engine._locator.find_target_window()
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


def run_headless() -> None:
    # 无 HUD 模式: 仅终端输出识别结果。
    from plugins.star_resonance_plugin.engines.game_state import GameState, GameStateManager
    from plugins.star_resonance_plugin.engines.automation import AutomationCore

    print('=' * 50)
    print('  SAO Auto — Headless 模式')
    print('  按 Ctrl+C 退出')
    print('=' * 50)

    settings = _settings()
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


def install_cli_runtime(ctx) -> None:
    # Register ``cli_test`` / ``cli_headless`` runtime hooks.
    #
    # The platform's ``main.py`` resolves these through the extension runtime
    # provider (see ``act_runtime_bridge``), so this is the only registration
    # entry the host needs.
    # The dispatch table in ``act_runtime_bridge.EXTENSION_RUNTIME_HANDLERS``
    # exposes these as ``cli_test`` / ``cli_headless`` to keep the table
    # in one place.
    pass