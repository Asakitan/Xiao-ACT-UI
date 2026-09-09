#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_main_panel.h"
#include "sao/ai_editor/ai_editor_settings_panel.h"
#include "sao/ui/input_router.h"
#include "sao/ui/linkstart_intro.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/panel.h"
#include "sao/ui/sound.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
constexpr UINT_PTR kUiService = 1;
void require(int32_t status) {
    if (status != 0)
        throw std::runtime_error("Production UI API status " + std::to_string(status));
}
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
struct Options {
    bool offline{};
    bool main_panel{};
    bool intro{};
    std::filesystem::path workspace{std::filesystem::current_path()};
    std::filesystem::path backend;
};
Options options() {
    Options value;
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
    if (!length || length >= 32768)
        throw std::runtime_error("Executable path unavailable");
    value.backend = std::filesystem::path(module).parent_path() / L"SaoAiEditor.exe";
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args)
        throw std::runtime_error("Arguments unavailable");
    struct ArgsRelease {
        LPWSTR* value;
        ~ArgsRelease() {
            LocalFree(value);
        }
    } release{args};
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = args[i];
        if (arg == L"--offline")
            value.offline = true;
        else if (arg == L"--main")
            value.main_panel = true;
        else if (arg == L"--intro")
            value.intro = true;
        else if ((arg == L"--workspace" || arg == L"--backend") && i + 1 < count) {
            auto path = std::filesystem::absolute(args[++i]);
            if (arg == L"--workspace")
                value.workspace = std::move(path);
            else
                value.backend = std::move(path);
        } else
            throw std::runtime_error("Usage: sao_ui_preview [--main] [--intro] [--offline] "
                                     "[--workspace PATH] [--backend EXE]");
    }
    return value;
}

// This host uses the real overlay/D3D compositor and real panel factories.
// Online mode borrows a production headless launcher; it has no mock RPC data.
struct Host {
    Options config;
    HWND window{};
    sao_ui_overlay_host_handle_t overlay{};
    sao_ui_compositor_handle_t compositor{};
    sao_ai_editor_launcher_t backend{};
    sao_ai_editor_settings_panel_t settings{};
    sao_ai_editor_main_panel_t main{};
    sao_ui_panel_handle_t panel{};
    sao_ui_input_router_deep_handle_t keyboard{};
    sao_ui_linkstart_handle_t intro{};
    ULONGLONG last_tick{};
    ULONGLONG last_service{};
    bool closing{};
    bool panel_hidden{};
    ULONGLONG close_started{};

    explicit Host(Options value) : config(std::move(value)) {}
    ~Host() {
        (void)close();
        (void)sao_ui_sound_shutdown();
    }
    int32_t close() noexcept {
        if (intro) {
            sao_ui_linkstart_destroy(intro);
            intro = nullptr;
        }
        // Do not hide a main panel with an active run: the operator still
        // needs its Stop action if retirement takes longer than the deadline.
        if (main) {
            const auto status = sao_ai_editor_main_panel_try_destroy(main);
            if (status)
                return status;
            main = nullptr;
            panel = nullptr;
        }
        if (!panel_hidden) {
            if (settings) {
                const auto status = sao_ai_editor_settings_panel_hide(settings);
                if (status)
                    return status;
            }
            if (main) {
                const auto status = sao_ai_editor_main_panel_hide(main);
                if (status)
                    return status;
            }
            panel_hidden = true;
        }
        if (settings) {
            const auto status = sao_ai_editor_settings_panel_try_destroy(settings);
            if (status)
                return status;
            settings = nullptr;
            panel = nullptr;
        }
        if (main) {
            const auto status = sao_ai_editor_main_panel_try_destroy(main);
            if (status)
                return status;
            main = nullptr;
            panel = nullptr;
        }
        if (keyboard) {
            const auto status = sao_ui_input_router_deep_try_destroy(keyboard);
            if (status)
                return status;
            keyboard = nullptr;
        }
        if (compositor) {
            const auto status = sao_ui_compositor_try_destroy(compositor);
            if (status)
                return status;
            compositor = nullptr;
        }
        if (overlay) {
            if (!sao_ui_overlay_host_destroy(overlay))
                return SAO_UI_STATUS_ERR_BUSY;
            overlay = nullptr;
        }
        if (backend) {
            int32_t exit_code = 0;
            (void)sao_ai_editor_shutdown(backend, 2000, &exit_code);
            sao_ai_editor_destroy(backend);
            backend = nullptr;
        }
        return 0;
    }
    void resize() {
        if (!overlay)
            return;
        if (IsIconic(window)) {
            require(sao_ui_overlay_host_set_visible(overlay, false));
            return;
        }
        RECT rect{};
        POINT origin{};
        if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin))
            return;
        if (rect.right < 1 || rect.bottom < 1)
            return;
        require(
            sao_ui_overlay_host_set_bounds(overlay, origin.x, origin.y, rect.right, rect.bottom));
        if (panel)
            require(sao_ui_panel_set_geometry(panel, 0, 0, rect.right, rect.bottom));
        require(sao_ui_overlay_host_set_visible(overlay, true));
    }
    void initialize() {
        if (!config.offline) {
            const auto executable = utf8(config.backend);
            const auto workspace = utf8(config.workspace);
            const std::string extra = "--headless --workspace \"" + workspace + "\"";
            SaoAiEditorLaunchConfig launch{};
            launch.executable_utf8 = executable.c_str();
            launch.base_dir_utf8 = workspace.c_str();
            launch.extra_args_utf8 = extra.c_str();
            launch.handshake_timeout_ms = 30000;
            require(sao_ai_editor_create(&launch, &backend));
            int32_t exit_code = 0;
            require(sao_ai_editor_launch(backend, &exit_code));
        }
        SaoOverlayHostConfig host{};
        host.width = 1280;
        host.height = 820;
        host.title_utf16 = L"SAO Classic — production compositor";
        require(sao_ui_overlay_host_create(&host, &overlay));
        require(sao_ui_compositor_create(overlay, nullptr, &compositor));
        if (config.main_panel) {
            require(sao_ai_editor_main_panel_create(compositor, backend, &main));
            require(sao_ai_editor_main_panel_show(main));
        } else {
            require(sao_ai_editor_settings_panel_create(compositor, backend, &settings));
            require(sao_ai_editor_settings_panel_show(settings));
        }
        require(sao_ui_panel_find_by_id(compositor,
                                        config.main_panel ? SAO_AI_EDITOR_MAIN_PANEL_ID
                                                          : SAO_AI_EDITOR_SETTINGS_PANEL_ID,
                                        &panel));
        require(sao_ui_input_router_deep_create(compositor, &keyboard));
        resize();
        if (config.intro) {
            RECT rect{};
            GetClientRect(window, &rect);
            SaoUiLinkStartConfig start{sizeof(SaoUiLinkStartConfig),
                                       static_cast<uint32_t>(rect.right),
                                       static_cast<uint32_t>(rect.bottom), 0, nullptr};
            require(sao_ui_linkstart_create(compositor, nullptr, &start, &intro));
            require(sao_ui_linkstart_show(intro));
        }
        last_tick = GetTickCount64();
        if (!SetTimer(window, kUiService, 16, nullptr))
            throw std::runtime_error("UI timer unavailable");
    }
    void service() {
        const auto now = GetTickCount64();
        if (closing) {
            if (close_started == 0)
                close_started = now;
            if (close() == 0) {
                DestroyWindow(window);
                return;
            }
            if (now - close_started > 5000) {
                closing = false;
                close_started = 0;
                panel_hidden = false;
                if (settings)
                    (void)sao_ai_editor_settings_panel_show(settings);
                SetWindowTextW(window, L"SAO Classic · 后台任务尚未结束，请停止任务后再关闭");
            }
            // Retrying a busy owner must drain RPC completions, just like the
            // production process owner. Nothing is freed until retirement.
        }
        if (now - last_service >= 50) {
            if (settings)
                require(sao_ai_editor_settings_panel_tick(settings));
            if (main)
                require(sao_ai_editor_main_panel_tick(main));
            last_service = now;
        }
        if (closing) {
            return;
        }
        if (intro) {
            bool active = false;
            require(sao_ui_linkstart_is_active(intro, &active));
            if (active && sao_ui_linkstart_tick(intro, static_cast<int32_t>(std::min<ULONGLONG>(
                                                           now - last_tick, INT32_MAX))) != 0)
                (void)sao_ui_linkstart_dismiss(intro);
        }
        last_tick = now;
        if (!IsIconic(window)) {
            auto status = sao_ui_compositor_tick(compositor);
            if (status && intro) {
                (void)sao_ui_linkstart_dismiss(intro);
                status = sao_ui_compositor_tick(compositor);
            }
            if (status != SAO_STATUS_ERR_DEVICE_LOST)
                require(status);
        }
        SaoPanelState state{};
        if (panel && sao_ui_panel_get_state(panel, &state) == 0 && !state.visible)
            closing = true;
    }
    bool key(const MSG& message) {
        if (!keyboard ||
            (message.message != WM_KEYDOWN && message.message != WM_KEYUP &&
             message.message != WM_CHAR && message.message != SAO_UI_NATIVE_TEXT_TAB_MESSAGE))
            return false;
        const HWND render = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        if (GetFocus() != render && GetFocus() != window)
            return false;
        bool consumed = false;
        const auto status = sao_ui_input_router_feed_raw_win32(
            keyboard, message.message, message.wParam, message.lParam, &consumed);
        if (status != SAO_STATUS_ERR_NOT_FOUND)
            require(status);
        return consumed;
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (!host)
        return DefWindowProcW(window, message, wp, lp);
    try {
        switch (message) {
        case WM_SIZE:
        case WM_MOVE:
            host->resize();
            return 0;
        case WM_TIMER:
            if (wp == kUiService)
                host->service();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {820, 620};
            return 0;
        case WM_CLOSE:
            host->closing = true;
            return 0;
        case WM_DESTROY:
            KillTimer(window, kUiService);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
    } catch (const std::exception& ex) {
        SetWindowTextA(window, ex.what());
        host->closing = true;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;
    int code = 1;
    try {
        Host host(options());
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = L"SaoProductionUiHost";
        if (!RegisterClassW(&cls))
            throw std::runtime_error("Window class unavailable");
        const wchar_t* title = host.config.offline
                                   ? L"SAO Classic · 生产渲染 / 离线模式（未连接后端）"
                                   : L"SAO Classic · 生产渲染与真实 AI Editor 后端";
        HWND window =
            CreateWindowExW(0, cls.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, 1280, 860, nullptr, nullptr, instance, &host);
        if (!window)
            throw std::runtime_error("Window creation failed");
        try {
            host.initialize();
            ShowWindow(window, show);
            MSG message{};
            BOOL received = 0;
            while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                if (!host.key(message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            code = received == 0 ? 0 : 1;
            if (IsWindow(window))
                DestroyWindow(window);
        } catch (...) {
            (void)host.close();
            DestroyWindow(window);
            throw;
        }
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "Production UI host", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return code;
}
