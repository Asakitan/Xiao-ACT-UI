# -*- coding: utf-8 -*-
# SAO Auto — 主程序入口
#
# UI 模式:
# webview — SAO WebView UI (pywebview, 唯一模式)
#
# 额外模式:
# --test      单次截图测试识别
# --headless  无 HUD，仅终端输出
#
# 架构:
# sao_webview.py  — WebView 透明 HUD (pywebview + EdgeChromium)
# config.py       — 平台配置
# act_platform/   — 插件 SDK、运行时分发、渲染钩子
# plugins/        — 游戏适配器动态注入游戏逻辑

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


# ── Windows MessageBox 兜底 excepthook ─────────────────────────────
# Nuitka --windows-console-mode=disable 下 sys.stderr 无处输出。任何主线程
# unhandled exception 会被 default excepthook 打到 stderr → 用户看到"点了没反应"。
# 装一个兜底 hook，把 traceback 用 MessageBoxW 弹出来。
# - 用 ctypes 直连 user32，避免依赖 tkinter/pywebview 才能报错的鸡生蛋
# - 只处理 Exception 子类（SystemExit / KeyboardInterrupt 保持默认行为）
# - traceback 截到 3800 chars 内避免 MessageBox 尺寸溢出
# - MB_ICONERROR + MB_OK + MB_SETFOREGROUND 保证前台弹出
# - 装完 sys.excepthook + threading.excepthook 都覆盖（后者 Python 3.8+）
# - --mcp-server 模式跳过（走 stdio JSON-RPC 需要 stderr 干净）
def _install_msgbox_excepthook():
    if '--mcp-server' in sys.argv:
        return
    try:
        import ctypes
        _u32 = ctypes.windll.user32
        _MB_OK = 0x00000000
        _MB_ICONERROR = 0x00000010
        _MB_SETFOREGROUND = 0x00010000
        _MB_TOPMOST = 0x00040000
        _FLAGS = _MB_OK | _MB_ICONERROR | _MB_SETFOREGROUND | _MB_TOPMOST
        _TITLE = "SAO Auto — 崩溃"

        def _fmt(exc_type, exc_value, exc_tb) -> str:
            import traceback as _tb
            try:
                lines = _tb.format_exception(exc_type, exc_value, exc_tb)
                body = "".join(lines)
            except Exception:
                body = f"{exc_type.__name__}: {exc_value}"
            head = (
                f"进程发生未捕获异常。\n"
                f"发生位置：{getattr(exc_value, 'args', ('',))}\n\n"
            )
            # MessageBox 硬上限约 32K，我们保守截到 3800 字符
            total = head + body
            if len(total) > 3800:
                total = total[:1900] + "\n\n[...traceback 截断...]\n\n" + total[-1800:]
            return total

        def _sys_hook(exc_type, exc_value, exc_tb):
            # SystemExit / KeyboardInterrupt / GeneratorExit 走默认，不弹
            if exc_type is not None and issubclass(exc_type, (SystemExit, KeyboardInterrupt, GeneratorExit)):
                sys.__excepthook__(exc_type, exc_value, exc_tb)
                return
            try:
                _u32.MessageBoxW(0, _fmt(exc_type, exc_value, exc_tb), _TITLE, _FLAGS)
            except Exception:
                pass
            # 让 default hook 也跑一遍（写 stderr —— 如果有 XIAOACT_DEBUG_LOG=1 会入日志文件）
            try:
                sys.__excepthook__(exc_type, exc_value, exc_tb)
            except Exception:
                pass

        sys.excepthook = _sys_hook

        # threading.Thread target 里 unhandled exception 走 threading.excepthook
        # (Python 3.8+)；补一份以覆盖 daemon 线程/helper spawn 线程等
        try:
            import threading as _th

            def _thread_hook(args):
                # args: threading.ExceptHookArgs(exc_type, exc_value, exc_traceback, thread)
                if args.exc_type is not None and issubclass(
                    args.exc_type, (SystemExit, KeyboardInterrupt, GeneratorExit)
                ):
                    return
                try:
                    thread_label = f"[线程 {args.thread.name}] "
                    msg = thread_label + _fmt(args.exc_type, args.exc_value, args.exc_traceback)
                    _u32.MessageBoxW(0, msg, _TITLE, _FLAGS)
                except Exception:
                    pass

            _th.excepthook = _thread_hook
        except Exception:
            pass
    except Exception:
        # user32 加载失败 (非 Windows) → 保持默认 excepthook, 不影响任何行为
        pass


_install_msgbox_excepthook()


# ── 全进程 hang detector (SAO_HANG_DETECTOR=1 默认开启) ─────────────
# 主人反馈: 主菜单跑着跑着卡死, 但 compositor watchdog 无报警. 说明卡的
# 是 Tk 主线程, compositor 内的 phase watchdog 抓不到. 装一个 faulthandler
# 定时器: 主线程每次机会打个心跳时间戳 (Tk after / compositor tick), 后
# 台 daemon 线程每 500ms 比对, 超过 SAO_HANG_THRESHOLD_MS (默认 2000ms)
# 无更新就 dump 所有线程栈到 stderr. 只 dump 一次直到心跳恢复.
def _install_hang_detector() -> None:
    if os.environ.get('SAO_HANG_DETECTOR', '1') != '1':
        return
    if '--mcp-server' in sys.argv:
        return
    try:
        threshold_ms = float(os.environ.get('SAO_HANG_THRESHOLD_MS', '2000'))
    except ValueError:
        threshold_ms = 2000.0
    try:
        import threading
        import time as _t
        import faulthandler as _fh

        _hb = [_t.perf_counter()]
        _dumped = [False]
        _paused = [False]

        def _heartbeat() -> None:
            _hb[0] = _t.perf_counter()
            _dumped[0] = False

        def _pause(paused: bool = True) -> None:
            # 主线程即将做一段合法的长阻塞 (比如 helper driver load 15-30s),
            # 期间 heartbeat 停 → watchdog 会疯狂 dump. pause 期间跳过检查.
            _paused[0] = paused
            if not paused:
                _hb[0] = _t.perf_counter()  # resume 时重置心跳

        # 暴露到 __main__, sao_gui 的 Tk after 循环可以调
        sys.modules['__main__']._sao_hang_heartbeat = _heartbeat
        sys.modules['__main__']._sao_hang_pause = _pause

        def _watchdog() -> None:
            while True:
                _t.sleep(0.5)
                if _paused[0]:
                    continue
                elapsed_ms = (_t.perf_counter() - _hb[0]) * 1000.0
                if elapsed_ms > threshold_ms and not _dumped[0]:
                    sys.stderr.write(
                        f'\n[HangDetector] 主线程 {elapsed_ms:.0f}ms 无心跳 '
                        f'(阈值 {threshold_ms:.0f}ms), dump 所有线程栈:\n')
                    sys.stderr.flush()
                    try:
                        _fh.dump_traceback(file=sys.stderr, all_threads=True)
                    except Exception:
                        pass
                    sys.stderr.write('\n')
                    sys.stderr.flush()
                    _dumped[0] = True
                elif elapsed_ms < threshold_ms:
                    _dumped[0] = False

        threading.Thread(target=_watchdog, name='HangDetector',
                         daemon=True).start()
    except Exception:
        pass


_install_hang_detector()


# Nuitka: optionally redirect all output to a log file (no console window).
# Off by default; set XIAOACT_DEBUG_LOG=1 before launch to enable.
# --mcp-server 模式依赖 stdout 做 JSON-RPC 通信，必须跳过重定向
_is_compiled = not os.path.isfile(os.path.abspath(__file__))
if _is_compiled:
    sys.frozen = True
    if '--mcp-server' not in sys.argv and os.environ.get('XIAOACT_DEBUG_LOG'):
        _log_path = os.path.join(os.path.dirname(os.path.abspath(sys.executable)), '_nuitka.log')
        try:
            _log_f = open(_log_path, 'w', encoding='utf-8', buffering=1)
            sys.stdout = _log_f
            sys.stderr = _log_f
        except Exception:
            pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def _bootstrap_runtime_overrides():
    # 模块化 onedir 布局适配 + update.exe 提升 (bootstrap).
    #
    # 在 PyInstaller onedir + noarchive=True + contents_directory='runtime' 下,
    # sys.path 只包含 runtime/。但 build_release.bat 会把 proto/ assets/ web/
    # icon.ico 提升到 EXE 顶层 (便于增量更新), 导致:
    # - 插件运行时的协议/资源目录不在 sys.path → 插件导入失败
    # - 开发时 sys.path 包含项目根, onefile 时 _MEIPASS 包含 proto/, 都正常
    # - **只有 onedir 打包后会 ImportError**
    # 解决: 把 EXE 所在目录 (frozen) / 当前文件目录 (dev) 加入 sys.path 头部。
    #
    # 同时:在最早时机调用 sao_updater.promote_runtime_update_exe(), 把
    # runtime/update.exe 提升到顶层 (旧 update.exe 通过嵌套路径绕过 _collect_entries)。
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
    # Dispatch a ``--test`` / ``--headless`` CLI action through the plugin runtime.
    #
    # The platform bootstraps the plugin manager (which triggers each plugin's
    # ``on_load`` and therefore ``register_extension_runtime``) and then looks up
    # the requested handler. The platform code never imports plugin modules.
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
    # Minimal owner for plugin-manager-driven CLI dispatch.
    #
    # ``main.py --test`` / ``--headless`` run before the full UI is online. We
    # bootstrap only the pieces the plugin manager needs (settings + an attrs
    # sink); the plugin's ``on_load`` then registers its runtime handlers via
    # ``register_extension_runtime``.

    def __init__(self) -> None:
        try:
            from config import SettingsManager
            self._cfg_settings_ref = SettingsManager()
        except Exception:
            self._cfg_settings_ref = None


def run_test():
    # Run the active plugin's one-shot CLI test handler.
    _dispatch_cli_runtime(
        "cli_test",
        "[SAO Auto] --test requires an active plugin runtime handler.",
    )


def run_headless():
    # Run the active plugin's headless CLI handler.
    _dispatch_cli_runtime(
        "cli_headless",
        "[SAO Auto] --headless requires an active plugin runtime handler.",
    )


def _start_update_check():
    # 在 UI 启动后后台检查一次更新；状态由 sao_updater 管理器维护，UI 会自行监听。
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
    # 注册 atexit hook：如果退出时有 staging 待应用包，就启动外部 helper 应用它。
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
    # 启动前显示 Tk 授权验证弹窗 (阻塞到用户关闭)
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
    # 根据 settings.json 中的 ui_mode 启动对应 UI.
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
    # v2.1.16: nudge process to ABOVE_NORMAL on Windows.
    #
    # Helps the Tk main loop + render lanes keep timeslices when many panels
    # are active and recognition/packet threads compete for CPU. ABOVE_NORMAL
    # is conservative — it doesn't starve background apps the way HIGH would.
    try:
        import ctypes
        ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)
    except Exception:
        pass


def _show_helper_bootstrap_failure(message: str) -> None:
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(
            0,
            message,
            "SAO Auto — Helper 启动失败",
            0x00000010 | 0x00010000 | 0x00040000,
        )
        return
    except Exception:
        pass
    try:
        print(message, file=sys.stderr, flush=True)
    except Exception:
        pass


def _early_hardening_bootstrap() -> bool:
    # All application modes synchronously establish authenticated helper IPC
    # before any UI or feature entry. Only paid mode proceeds to CMD_INIT.
    try:
        try:
            from license._bootstrap import receive_session_key
            receive_session_key(strict=False)
        except Exception:
            pass

        _is_paid = False
        try:
            from license import get_license_manager
            _lm = get_license_manager()
            _is_paid = bool(getattr(_lm, 'is_paid', False))
        except Exception:
            _is_paid = False

        try:
            from license._integrity_ext import verify_key_dlls
            verify_key_dlls(hard_fail=False)
        except Exception:
            pass
        try:
            from license.anti_hook import full_anti_hook_check
            full_anti_hook_check()
        except Exception:
            pass

        import time as _t
        _pause_fn = getattr(sys.modules.get('__main__'),
                            '_sao_hang_pause', None)
        if callable(_pause_fn):
            _pause_fn(True)
        try:
            from mem_probe import rt_io_proxy
            _t0 = _t.time()
            if not rt_io_proxy.ensure_helper_ready(timeout=30.0):
                _show_helper_bootstrap_failure(
                    "SAO Auto helper 未能完成认证连接，程序将退出。"
                )
                return False
            print(f"[main] helper IPC ready in {_t.time()-_t0:.1f}s", flush=True)
            if _is_paid and not rt_io_proxy.ensure_loaded():
                _show_helper_bootstrap_failure(
                    "SAO Auto helper 驱动初始化失败，程序将退出。"
                )
                return False
            return True
        except Exception as exc:
            _show_helper_bootstrap_failure(
                f"SAO Auto helper 启动失败: {type(exc).__name__}: {exc}"
            )
            return False
        finally:
            if callable(_pause_fn):
                _pause_fn(False)
    except Exception as exc:
        _show_helper_bootstrap_failure(
            f"SAO Auto 启动预检失败: {type(exc).__name__}: {exc}"
        )
        return False


def main():
    # ！打包版静默失败根因防御 (双层)！
    # launcher (linkstart.exe) 通过 --sao-ipc-mapping <name> <total> <wo> <wl>
    # <so> <sl> <mko> <mkl> [pubkey_hex] 派发 IPC。argparse.parse_args() 遇到
    # 未知参数会立即 exit(2)，打包版 --windows-console-mode=disable → stderr
    # 无处输出 → 主程静默退出，UI 一次都不出现。
    #
    # 首选: strip_ipc_marker_early() 完整 parse + 缓存供 receive_session_key 复用。
    # 兜底: 若 license 子系统 import 失败（比如 cryptography 未打包），仍要保证
    #      argparse 干净，走 inline strip 不依赖任何三方模块。这一层是"UI 起得来"
    #      的绝对底线，宁可丢 marker info 走文件回退也不能让主程 exit。
    try:
        from license._bootstrap import strip_ipc_marker_early
        strip_ipc_marker_early()
    except Exception:
        try:
            for _i, _tok in enumerate(sys.argv):
                if _tok == "--sao-ipc-mapping":
                    # marker 后跟随的 tokens 都不以 '--' 开头（都是 name/数字/hex），
                    # 一直吃到下一个 '--' 参数或 argv 末尾
                    _j = _i + 1
                    while _j < len(sys.argv) and not sys.argv[_j].startswith("--"):
                        _j += 1
                    sys.argv = sys.argv[:_i] + sys.argv[_j:]
                    break
        except Exception:
            pass

    parser = argparse.ArgumentParser(description='SAO Auto — 游戏 HUD 与自动化')
    parser.add_argument('--test', action='store_true', help='单次识别测试')
    parser.add_argument('--headless', action='store_true', help='无 HUD 终端模式')
    parser.add_argument('--ai-editor', action='store_true',
                        help='启动 AI Editor 独立窗口 (pywebview, 不进入主 UI)')
    parser.add_argument('--workshop', action='store_true',
                        help='启动创意工坊独立窗口 (pywebview, 不进入主 UI)')
    parser.add_argument('--mcp-server', action='store_true',
                        help='启动 AI Editor MCP 服务器 (stdio, 供外部 IDE AI 接入)')
    parser.add_argument('--mcp-port', type=int, default=0,
                        help='MCP 服务器 HTTP 模式端口 (配合 --mcp-server 使用)')
    args = parser.parse_args()

    if args.mcp_server:
        # MCP server 是本机 stdio 开发通道，跳过 hardening bootstrap
        from ai_editor.mcp_server import McpServer, McpHttpServer
        if args.mcp_port:
            McpHttpServer(port=args.mcp_port).run()
        else:
            McpServer().run()
        return

    # 所有模式 (test/headless/ai-editor/workshop/run_ui) 统一先跑 hardening bootstrap
    if not _early_hardening_bootstrap():
        return

    _set_dpi_aware()
    _elevate_process_priority()
    _register_apply_on_exit()
    _start_update_check()

    if args.ai_editor:
        if getattr(sys, 'frozen', False):
            os.environ['PYWEBVIEW_GUI'] = 'edgechromium'
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
