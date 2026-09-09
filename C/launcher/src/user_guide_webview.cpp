// SAO Auto — launcher/user_guide_webview.cpp
//
// In-process WebView2 host for the offline user guide.  Loads
// WebView2Loader.dll dynamically (same pattern as the AI Editor webview
// bridge) so a machine without the WebView2 runtime keeps working via the
// ShellExecute fallback.  A dedicated STA thread owns the window, COM
// apartment, WebView objects, callbacks, and message loop.  Environment
// creation tries a writable LocalAppData profile, then Temp, before the
// default-browser fallback.

#include "sao/launcher/user_guide_webview.h"
#include "sao/ui/sound.h"

#include <windows.h>
#include <combaseapi.h>
#include <objbase.h>
#include <process.h>
#include <shellapi.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#if defined(SAO_LAUNCHER_HAS_WEBVIEW2)
#include <WebView2.h>
#include <wrl/async.h>
#include <wrl/client.h>
#endif

namespace {

#if defined(SAO_LAUNCHER_HAS_WEBVIEW2)

using Microsoft::WRL::ComPtr;

using CreateEnvironmentFn = HRESULT(WINAPI*)(
    PCWSTR environment_options,
    PCWSTR user_data_folder,
    ICoreWebView2EnvironmentOptions* environment_options_struct,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

constexpr wchar_t kWindowClassName[] = L"SAO_USER_GUIDE_WEBVIEW_WND";
constexpr wchar_t kWindowTitle[] = L"SAO Auto \u2014 \u7528\u6237\u6307\u5357";
constexpr UINT kShowWindowMessage = WM_APP + 0x47u;
constexpr UINT kRefreshSoundPolicyMessage = WM_APP + 0x48u;
constexpr DWORD kStartupWaitMs = 5000u;
constexpr DWORD kShutdownWaitMs = 5000u;

enum class GuideHostPhase : uint32_t {
    idle = 0u,
    starting = 1u,
    running = 2u,
    stopping = 3u,
};

std::mutex g_host_mutex;
HANDLE g_thread_handle = nullptr;
std::atomic<HWND> g_published_window{nullptr};
std::atomic<DWORD> g_thread_id{0u};
std::atomic<GuideHostPhase> g_host_phase{GuideHostPhase::idle};

struct StartupContext {
    std::atomic<long> references{2};
    HANDLE ready_event = nullptr;
    std::atomic<bool> started{false};
    std::atomic<bool> cancel_requested{false};
    std::wstring url;
    std::wstring fallback_path;
};

struct GuideState {
    HWND window = nullptr;
    bool closing = false;
    bool fallback_started = false;
    std::wstring fallback_path;
    std::wstring retry_user_data_folder;
    bool user_data_retry_started = false;
    CreateEnvironmentFn create_environment = nullptr;
    std::shared_ptr<void> loader_lease;
    ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
        environment_handler;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navigation_completed_token{};
    bool navigation_handler_registered = false;
    EventRegistrationToken web_message_token{};
    bool web_message_handler_registered = false;
    std::wstring guide_url;
    sao_ui_sound_group_t sound_group{};
    ~GuideState() {
        if (sound_group != 0)
            (void)sao_ui_sound_group_destroy(sound_group);
    }
};

void routeGuideSound(GuideState& state, std::wstring_view message) noexcept {
    if (message == L"sao-guide-sfx-stop") {
        if (state.sound_group != 0)
            (void)sao_ui_sound_group_stop(state.sound_group);
        return;
    }
    constexpr std::wstring_view prefix = L"sao-guide-sfx-play:";
    if (!message.starts_with(prefix) || message.size() > 64)
        return;
    message.remove_prefix(prefix.size());
    const size_t delimiter = message.find(L':');
    if (delimiter == std::wstring_view::npos)
        return;
    const auto name = message.substr(0, delimiter);
    const auto gain = message.substr(delimiter + 1);
    if (gain.empty() || gain.size() > 3)
        return;
    int volume = 0;
    for (wchar_t character : gain) {
        if (character < L'0' || character > L'9')
            return;
        volume = volume * 10 + character - L'0';
    }
    if (volume > 100)
        return;
    SaoUiSoundCue cue = SAO_UI_SOUND_COUNT;
    if (name == L"click")
        cue = SAO_UI_SOUND_CLICK;
    else if (name == L"menu_open")
        cue = SAO_UI_SOUND_MENU_OPEN;
    else if (name == L"menu_close")
        cue = SAO_UI_SOUND_MENU_CLOSE;
    else if (name == L"submenu")
        cue = SAO_UI_SOUND_SUBMENU;
    else if (name == L"panel")
        cue = SAO_UI_SOUND_PANEL;
    else if (name == L"alert_close")
        cue = SAO_UI_SOUND_ALERT_CLOSE;
    if (cue == SAO_UI_SOUND_COUNT)
        return;
    if (state.sound_group == 0 && sao_ui_sound_group_create(&state.sound_group) != SAO_STATUS_OK)
        return;
    (void)sao_ui_sound_play_in_group(cue, volume, state.sound_group);
}

void releaseStartupContext(StartupContext* context) noexcept {
    if (context == nullptr || context->references.fetch_sub(1) != 1)
        return;
    if (context->ready_event != nullptr)
        CloseHandle(context->ready_event);
    delete context;
}

void signalStartup(StartupContext* context, bool started) noexcept {
    context->started.store(started, std::memory_order_release);
    SetEvent(context->ready_event);
    releaseStartupContext(context);
}

void clearPublishedWindow(HWND window) noexcept {
    HWND expected = window;
    (void)g_published_window.compare_exchange_strong(expected, nullptr);
}

void removeNavigationHandler(GuideState& state) noexcept {
    if (state.webview && state.navigation_handler_registered) {
        (void)state.webview->remove_NavigationCompleted(
            state.navigation_completed_token);
    }
    state.navigation_handler_registered = false;
    state.navigation_completed_token = {};
}

void removeWebMessageHandler(GuideState& state) noexcept {
    if (state.webview && state.web_message_handler_registered) {
        (void)state.webview->remove_WebMessageReceived(state.web_message_token);
    }
    state.web_message_handler_registered = false;
    state.web_message_token = {};
}

void postSoundPolicy(GuideState* state) noexcept {
    if (state == nullptr || !state->webview || state->closing)
        return;
    bool enabled = true;
    int32_t volume = 70;
    (void)sao_ui_sound_get_enabled(&enabled);
    (void)sao_ui_sound_get_volume(&volume);
    const std::wstring policy = std::wstring(L"{\"type\":\"sao-guide-sfx-policy\",\"enabled\":") +
                                (enabled ? L"true" : L"false") + L",\"volume\":" +
                                std::to_wstring(std::clamp(volume, 0, 100)) + L"}";
    (void)state->webview->PostWebMessageAsJson(policy.c_str());
}

void closeController(GuideState& state) noexcept {
    if (state.sound_group != 0)
        (void)sao_ui_sound_group_stop(state.sound_group);
    removeWebMessageHandler(state);
    removeNavigationHandler(state);
    state.webview.Reset();
    if (state.controller) {
        (void)state.controller->Close();
        state.controller.Reset();
    }
    state.environment.Reset();
    state.environment_handler.Reset();
}

void requestWindowClose(const std::shared_ptr<GuideState>& state) noexcept {
    if (state->window != nullptr && IsWindow(state->window))
        (void)PostMessageW(state->window, WM_CLOSE, 0, 0);
}

void fallbackAndClose(const std::shared_ptr<GuideState>& state) noexcept {
    if (!state->closing && !state->fallback_started &&
        !state->fallback_path.empty()) {
        state->fallback_started = true;
        (void)ShellExecuteW(nullptr, L"open", state->fallback_path.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
    }
    g_host_phase.store(GuideHostPhase::stopping,
                       std::memory_order_release);
    requestWindowClose(state);
}

LRESULT CALLBACK guideWindowProc(HWND window, UINT message,
                                 WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<GuideState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        state = static_cast<GuideState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        state->window = window;
        g_published_window.store(window, std::memory_order_release);
    }

    switch (message) {
        case WM_SIZE:
            if (state != nullptr && state->controller &&
                w_param != SIZE_MINIMIZED) {
                RECT bounds{};
                GetClientRect(window, &bounds);
                (void)state->controller->put_Bounds(bounds);
            }
            return 0;
        case kShowWindowMessage:
            ShowWindow(window, SW_RESTORE);
            (void)SetForegroundWindow(window);
            return 0;
        case kRefreshSoundPolicyMessage:
            postSoundPolicy(state);
            return 0;
        case WM_CLOSE:
            if (state != nullptr)
                state->closing = true;
            g_host_phase.store(GuideHostPhase::stopping,
                               std::memory_order_release);
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (state != nullptr) {
                state->closing = true;
                closeController(*state);
                state->window = nullptr;
            }
            clearPublishedWindow(window);
            g_host_phase.store(GuideHostPhase::stopping,
                               std::memory_order_release);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w_param, l_param);
    }
}

std::wstring toFileUri(const wchar_t* path) {
    std::wstring uri = L"file:///";
    for (const wchar_t* c = path; *c != L'\0'; ++c) {
        if (*c == L'\\') {
            uri += L'/';
        } else if (*c == L'%') {
            uri += L"%25";
        } else if (*c == L'#') {
            uri += L"%23";
        } else if (*c == L' ') {
            uri += L"%20";
        } else {
            uri += *c;
        }
    }
    return uri;
}

std::wstring loaderPath() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length > MAX_PATH) return L"WebView2Loader.dll";
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"WebView2Loader.dll";
    return path.substr(0, slash + 1) + L"WebView2Loader.dll";
}

std::wstring tryUserDataFolder(const wchar_t* base, DWORD length) {
    if (base == nullptr || length == 0u || length >= 32768u)
        return {};
    std::wstring path(base, length);
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    path += L"SaoAuto.UserGuide.WebView2";
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            return {};
        }
    }

    std::wstring probe_path = path;
    probe_path += L"\\.sao-write-probe-";
    probe_path += std::to_wstring(GetCurrentProcessId());
    probe_path += L"-";
    probe_path += std::to_wstring(GetCurrentThreadId());
    probe_path += L".tmp";
    const HANDLE probe = CreateFileW(
        probe_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (probe == INVALID_HANDLE_VALUE)
        return {};
    CloseHandle(probe);
    return path;
}

struct UserDataFolderCandidates {
    std::wstring primary;
    std::wstring retry;
};

UserDataFolderCandidates userDataFolderPaths() {
    UserDataFolderCandidates candidates;
    wchar_t base[32768]{};
    DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", base, static_cast<DWORD>(_countof(base)));
    candidates.primary = tryUserDataFolder(base, length);

    base[0] = L'\0';
    length = GetTempPathW(static_cast<DWORD>(_countof(base)), base);
    std::wstring temporary = tryUserDataFolder(base, length);
    if (candidates.primary.empty()) {
        candidates.primary = std::move(temporary);
    } else if (!temporary.empty() &&
               _wcsicmp(candidates.primary.c_str(), temporary.c_str()) != 0) {
        candidates.retry = std::move(temporary);
    }
    return candidates;
}

bool registerWindowClass(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, kWindowClassName, &existing) != 0)
        return existing.lpfnWndProc == guideWindowProc;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = guideWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassExW(&wc) != 0;
}

void reapGuideThreadLocked() noexcept {
    if (g_thread_handle == nullptr ||
        WaitForSingleObject(g_thread_handle, 0) != WAIT_OBJECT_0) {
        return;
    }
    CloseHandle(g_thread_handle);
    g_thread_handle = nullptr;
    g_published_window.store(nullptr, std::memory_order_release);
    g_thread_id.store(0u, std::memory_order_release);
    g_host_phase.store(GuideHostPhase::idle, std::memory_order_release);
}

unsigned __stdcall guideThreadMain(void* parameter) {
    auto* startup = static_cast<StartupContext*>(parameter);
    bool startup_pending = true;
    bool com_initialized = false;
    HMODULE unleased_loader = nullptr;
    std::shared_ptr<void> loader_lease;
    std::shared_ptr<GuideState> state;

    auto complete_startup = [&](bool started) noexcept {
        if (!startup_pending)
            return;
        startup_pending = false;
        signalStartup(startup, started);
    };
    auto cleanup = [&]() noexcept {
        if (state) {
            if (state->window != nullptr && IsWindow(state->window)) {
                state->closing = true;
                DestroyWindow(state->window);
            }
            closeController(*state);
            state.reset();
        }
        loader_lease.reset();
        if (unleased_loader != nullptr)
            FreeLibrary(unleased_loader);
        g_published_window.store(nullptr, std::memory_order_release);
        g_thread_id.store(0u, std::memory_order_release);
        g_host_phase.store(GuideHostPhase::idle, std::memory_order_release);
        if (com_initialized)
            CoUninitialize();
    };

    try {
        std::wstring url;
        url.swap(startup->url);
        std::wstring fallback_path;
        fallback_path.swap(startup->fallback_path);

        MSG queue_probe{};
        (void)PeekMessageW(&queue_probe, nullptr, WM_USER, WM_USER,
                           PM_NOREMOVE);
        g_thread_id.store(GetCurrentThreadId(), std::memory_order_release);

        const HRESULT com_status =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com_status)) {
            complete_startup(false);
            cleanup();
            return 0u;
        }
        com_initialized = true;

        if (startup->cancel_requested.load(std::memory_order_acquire)) {
            complete_startup(false);
            cleanup();
            return 0u;
        }

        unleased_loader = LoadLibraryW(loaderPath().c_str());
        if (unleased_loader == nullptr) {
            complete_startup(false);
            cleanup();
            return 0u;
        }
        loader_lease = std::shared_ptr<void>(
            unleased_loader, [](void* module) noexcept {
                if (module != nullptr)
                    FreeLibrary(reinterpret_cast<HMODULE>(module));
            });
        HMODULE loader = unleased_loader;
        unleased_loader = nullptr;
        const auto create_environment = reinterpret_cast<CreateEnvironmentFn>(
            GetProcAddress(loader,
                           "CreateCoreWebView2EnvironmentWithOptions"));
        if (create_environment == nullptr) {
            complete_startup(false);
            cleanup();
            return 0u;
        }

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registerWindowClass(instance)) {
            complete_startup(false);
            cleanup();
            return 0u;
        }

        state = std::make_shared<GuideState>();
        state->fallback_path = std::move(fallback_path);
        state->create_environment = create_environment;
        state->loader_lease = loader_lease;
        state->guide_url = url;
        const auto url_value =
            std::make_shared<const std::wstring>(std::move(url));

        constexpr int width = 960;
        constexpr int height = 780;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        HWND window = CreateWindowExW(
            0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y,
            width, height, nullptr, nullptr, instance, state.get());
        if (window == nullptr) {
            complete_startup(false);
            cleanup();
            return 0u;
        }
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);

        UserDataFolderCandidates user_data_folders = userDataFolderPaths();
        if (user_data_folders.primary.empty() ||
            startup->cancel_requested.load(std::memory_order_acquire)) {
            complete_startup(false);
            cleanup();
            return 0u;
        }
        state->retry_user_data_folder =
            std::move(user_data_folders.retry);

        state->environment_handler = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [state, url_value](
                HRESULT result,
                ICoreWebView2Environment* environment) -> HRESULT {
                if (state->closing || state->window == nullptr)
                    return S_OK;
                if (FAILED(result) || environment == nullptr) {
                    if (!state->user_data_retry_started &&
                        !state->retry_user_data_folder.empty()) {
                        state->user_data_retry_started = true;
                        const HRESULT retry_status = state->create_environment(
                            nullptr, state->retry_user_data_folder.c_str(),
                            nullptr, state->environment_handler.Get());
                        if (SUCCEEDED(retry_status))
                            return S_OK;
                    }
                    fallbackAndClose(state);
                    return S_OK;
                }

                state->environment_handler.Reset();
                state->environment = environment;
                auto controller_handler = Microsoft::WRL::Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [state, url_value](
                        HRESULT controller_result,
                        ICoreWebView2Controller* controller) -> HRESULT {
                        if (state->closing || state->window == nullptr)
                            return S_OK;
                        if (FAILED(controller_result) ||
                            controller == nullptr) {
                            fallbackAndClose(state);
                            return S_OK;
                        }

                        state->controller = controller;
                        RECT bounds{};
                        GetClientRect(state->window, &bounds);
                        (void)controller->put_Bounds(bounds);

                        ComPtr<ICoreWebView2> webview;
                        if (FAILED(controller->get_CoreWebView2(&webview)) ||
                            !webview) {
                            fallbackAndClose(state);
                            return S_OK;
                        }
                        state->webview = webview;
                        ComPtr<ICoreWebView2Settings> web_settings;
                        if (SUCCEEDED(webview->get_Settings(&web_settings)) && web_settings)
                            (void)web_settings->put_IsWebMessageEnabled(TRUE);

                        auto web_message_handler =
                            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [state](ICoreWebView2*,
                                        ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    if (state->closing || args == nullptr)
                                        return S_OK;
                                    LPWSTR source = nullptr;
                                    LPWSTR message = nullptr;
                                    const HRESULT source_status = args->get_Source(&source);
                                    const HRESULT message_status =
                                        args->TryGetWebMessageAsString(&message);
                                    const std::wstring_view source_view = source ? source : L"";
                                    const bool local_guide =
                                        SUCCEEDED(source_status) && source != nullptr &&
                                        source_view.substr(0, source_view.find(L'#')) ==
                                            state->guide_url;
                                    const bool requested =
                                        SUCCEEDED(message_status) && message != nullptr &&
                                        (std::wcscmp(message, L"sao-guide-sfx-ready") == 0 ||
                                         std::wcscmp(message, L"sao-guide-sfx-refresh") == 0);
                                    if (local_guide && SUCCEEDED(message_status) &&
                                        message != nullptr)
                                        routeGuideSound(*state, message);
                                    if (source != nullptr)
                                        CoTaskMemFree(source);
                                    if (message != nullptr)
                                        CoTaskMemFree(message);
                                    if (local_guide && requested)
                                        postSoundPolicy(state.get());
                                    return S_OK;
                                });
                        EventRegistrationToken web_message_token{};
                        if (!web_message_handler ||
                            FAILED(webview->add_WebMessageReceived(web_message_handler.Get(),
                                                                   &web_message_token))) {
                            fallbackAndClose(state);
                            return S_OK;
                        }
                        state->web_message_token = web_message_token;
                        state->web_message_handler_registered = true;

                        auto navigation_handler = Microsoft::WRL::Callback<
                            ICoreWebView2NavigationCompletedEventHandler>(
                            [state](
                                ICoreWebView2*,
                                ICoreWebView2NavigationCompletedEventArgs*
                                    args) -> HRESULT {
                                if (state->closing || state->window == nullptr)
                                    return S_OK;
                                BOOL succeeded = FALSE;
                                const HRESULT status =
                                    args == nullptr
                                    ? E_POINTER
                                    : args->get_IsSuccess(&succeeded);
                                removeNavigationHandler(*state);
                                if (FAILED(status) || succeeded == FALSE) {
                                    fallbackAndClose(state);
                                } else {
                                    g_host_phase.store(
                                        GuideHostPhase::running,
                                        std::memory_order_release);
                                    postSoundPolicy(state.get());
                                }
                                return S_OK;
                            });
                        EventRegistrationToken token{};
                        if (!navigation_handler ||
                            FAILED(webview->add_NavigationCompleted(
                                navigation_handler.Get(), &token))) {
                            fallbackAndClose(state);
                            return S_OK;
                        }
                        state->navigation_completed_token = token;
                        state->navigation_handler_registered = true;
                        if (FAILED(webview->Navigate(url_value->c_str())))
                            fallbackAndClose(state);
                        return S_OK;
                    });
                if (!controller_handler ||
                    FAILED(environment->CreateCoreWebView2Controller(
                        state->window, controller_handler.Get()))) {
                    fallbackAndClose(state);
                }
                return S_OK;
            });
        HRESULT environment_status =
            state->environment_handler
            ? create_environment(nullptr, user_data_folders.primary.c_str(),
                                 nullptr, state->environment_handler.Get())
            : E_OUTOFMEMORY;
        if (FAILED(environment_status) &&
            !state->retry_user_data_folder.empty()) {
            state->user_data_retry_started = true;
            environment_status = create_environment(
                nullptr, state->retry_user_data_folder.c_str(), nullptr,
                state->environment_handler.Get());
        }
        if (FAILED(environment_status) ||
            startup->cancel_requested.load(std::memory_order_acquire)) {
            complete_startup(false);
            cleanup();
            return 0u;
        }

        complete_startup(true);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } catch (...) {
        if (startup_pending) {
            complete_startup(false);
        } else if (state && !state->closing) {
            fallbackAndClose(state);
        }
    }

    cleanup();
    return 0u;
}

#endif  // SAO_LAUNCHER_HAS_WEBVIEW2

}  // namespace

namespace sao::launcher {

void refreshUserGuideSoundPolicy() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2)
    const HWND window = g_published_window.load(std::memory_order_acquire);
    if (window != nullptr && IsWindow(window))
        (void)PostMessageW(window, kRefreshSoundPolicyMessage, 0, 0);
#endif
}

bool openUserGuideInWebView(const wchar_t* docs_index_path) noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2)
    if (docs_index_path == nullptr || docs_index_path[0] == L'\0')
        return false;

    StartupContext* startup = nullptr;
    bool thread_started = false;
    try {
        std::unique_lock<std::mutex> lock(g_host_mutex);
        reapGuideThreadLocked();
        const GuideHostPhase existing_phase =
            g_host_phase.load(std::memory_order_acquire);
        if (g_thread_handle != nullptr &&
            existing_phase != GuideHostPhase::starting &&
            existing_phase != GuideHostPhase::running) {
            if (WaitForSingleObject(g_thread_handle, kShutdownWaitMs) !=
                WAIT_OBJECT_0) {
                return false;
            }
            reapGuideThreadLocked();
        }
        if (g_thread_handle != nullptr) {
            const HWND window =
                g_published_window.load(std::memory_order_acquire);
            lock.unlock();
            if (window != nullptr)
                (void)PostMessageW(window, kShowWindowMessage, 0, 0);
            return true;
        }

        startup = new (std::nothrow) StartupContext();
        if (startup == nullptr)
            return false;
        startup->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (startup->ready_event == nullptr) {
            releaseStartupContext(startup);
            releaseStartupContext(startup);
            startup = nullptr;
            return false;
        }
        startup->url = toFileUri(docs_index_path);
        startup->fallback_path = docs_index_path;

        unsigned thread_id = 0u;
        g_host_phase.store(GuideHostPhase::starting,
                           std::memory_order_release);
        const uintptr_t thread = _beginthreadex(
            nullptr, 0u, guideThreadMain, startup, 0u, &thread_id);
        if (thread == 0u) {
            g_host_phase.store(GuideHostPhase::idle,
                               std::memory_order_release);
            releaseStartupContext(startup);
            releaseStartupContext(startup);
            startup = nullptr;
            return false;
        }
        thread_started = true;
        g_thread_handle = reinterpret_cast<HANDLE>(thread);
        const HANDLE ready_event = startup->ready_event;

        const DWORD wait_result =
            WaitForSingleObject(ready_event, kStartupWaitMs);
        if (wait_result != WAIT_OBJECT_0) {
            startup->cancel_requested.store(true, std::memory_order_release);
            g_host_phase.store(GuideHostPhase::stopping,
                               std::memory_order_release);
            const HWND window =
                g_published_window.load(std::memory_order_acquire);
            const DWORD host_thread_id =
                g_thread_id.load(std::memory_order_acquire);
            if (window != nullptr) {
                (void)PostMessageW(window, WM_CLOSE, 0, 0);
            } else if (host_thread_id != 0u) {
                (void)PostThreadMessageW(host_thread_id, WM_QUIT, 0, 0);
            }
            releaseStartupContext(startup);
            startup = nullptr;
            return false;
        }

        const bool started =
            startup->started.load(std::memory_order_acquire);
        releaseStartupContext(startup);
        startup = nullptr;
        return started;
    } catch (...) {
        if (startup != nullptr) {
            if (thread_started) {
                startup->cancel_requested.store(true,
                                                std::memory_order_release);
                g_host_phase.store(GuideHostPhase::stopping,
                                   std::memory_order_release);
                const HWND window =
                    g_published_window.load(std::memory_order_acquire);
                const DWORD host_thread_id =
                    g_thread_id.load(std::memory_order_acquire);
                if (window != nullptr) {
                    (void)PostMessageW(window, WM_CLOSE, 0, 0);
                } else if (host_thread_id != 0u) {
                    (void)PostThreadMessageW(host_thread_id, WM_QUIT, 0, 0);
                }
                releaseStartupContext(startup);
            } else {
                releaseStartupContext(startup);
                releaseStartupContext(startup);
            }
        }
        return false;
    }
#else
    (void)docs_index_path;
    return false;
#endif
}

bool shutdownUserGuideWebView() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2)
    try {
        std::unique_lock<std::mutex> lock(g_host_mutex);
        reapGuideThreadLocked();
        if (g_thread_handle == nullptr)
            return true;

        const HANDLE thread = g_thread_handle;
        const HWND window =
            g_published_window.load(std::memory_order_acquire);
        const DWORD thread_id = g_thread_id.load(std::memory_order_acquire);
        g_host_phase.store(GuideHostPhase::stopping,
                           std::memory_order_release);
        bool posted = false;
        if (window != nullptr)
            posted = PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
        if (!posted && thread_id != 0u)
            (void)PostThreadMessageW(thread_id, WM_QUIT, 0, 0);

        if (WaitForSingleObject(thread, kShutdownWaitMs) != WAIT_OBJECT_0)
            return false;

        reapGuideThreadLocked();
        return g_thread_handle == nullptr;
    } catch (...) {
        return false;
    }
#else
    return true;
#endif
}

} // namespace sao::launcher
