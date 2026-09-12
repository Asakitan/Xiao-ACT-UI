#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "sao/launcher/user_guide_webview.h"

#if defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#include "sao/ui/panel.h"
#include "sao/ui/sound.h"
#include <nlohmann/json.hpp>
#endif

#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <source_location>

#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
#include <WebView2.h>
#include <wrl.h>

namespace {
using Microsoft::WRL::ComPtr;
using nlohmann::json;
using CreateEnvironmentFn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

struct GuideState {
    sao_ui_compositor_handle_t compositor{};
    sao_ui_composition_slot_handle_t slot{};
    sao_ui_panel_handle_t status_panel{};
    sao_ui_sound_group_t sound_group{};
    HWND parent{};
    DWORD owner{};
    HMODULE loader{};
    CreateEnvironmentFn create_environment{};
    bool com_initialized{}, ready{}, presented{}, failed{}, closing{}, retry_requested{};
    bool visible{true};
    bool native_intro_completed{}, profile_retried{};
    uint32_t pending_async{}, callback_depth{}, mouse_buttons{};
    POINT mouse{};
    uint64_t target_generation{};
    ULONGLONG startup_deadline{};
    std::wstring path, url, retry_profile;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2CompositionController> composition;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2Controller3> controller3;
    ComPtr<ICoreWebView2> view;
    EventRegistrationToken navigation_starting{}, navigation_completed{}, web_message{};
    EventRegistrationToken cursor_changed{}, process_failed{}, new_window{};
    bool navigation_starting_set{}, navigation_completed_set{}, web_message_set{};
    bool cursor_changed_set{}, process_failed_set{}, new_window_set{};
    int32_t width{}, height{}, host_x{}, host_y{};
    uint32_t dpi{96};

    ~GuideState() {
        view.Reset();
        controller3.Reset();
        if (controller) (void)controller->Close();
        controller.Reset();
        composition.Reset();
        environment.Reset();
        if (sound_group) (void)sao_ui_sound_group_destroy(sound_group);
        if (loader) FreeLibrary(loader);
        if (com_initialized) CoUninitialize();
    }
};

std::atomic<std::shared_ptr<GuideState>> g_guide;
struct CallbackScope {
    GuideState& state;
    explicit CallbackScope(GuideState& value) : state(value) { ++state.callback_depth; }
    ~CallbackScope() { --state.callback_depth; }
};

bool transient(sao_status_t status) noexcept {
    return status == SAO_STATUS_ERR_DEVICE_LOST || status == SAO_STATUS_ERR_NOT_INITIALIZED ||
           status == SAO_STATUS_ERR_CANCELLED;
}

std::wstring file_uri(const wchar_t* path) {
    std::wstring result = L"file:///";
    for (; *path; ++path) {
        switch (*path) {
        case L'\\': result += L'/'; break;
        case L'%': result += L"%25"; break;
        case L'#': result += L"%23"; break;
        case L'?': result += L"%3F"; break;
        case L' ': result += L"%20"; break;
        default: result += *path; break;
        }
    }
    return result;
}

bool same_document(const GuideState& state, const wchar_t* uri) noexcept {
    if (!uri) return false;
    const std::wstring_view value(uri);
    return value.substr(0, value.find(L'#')) == state.url;
}

std::wstring profile_path(bool temporary) {
    wchar_t base[32768]{};
    const DWORD count = temporary ? GetTempPathW(_countof(base), base)
        : GetEnvironmentVariableW(L"LOCALAPPDATA", base, _countof(base));
    if (!count || count >= _countof(base)) return {};
    std::wstring path(base, count);
    if (path.back() != L'\\') path += L'\\';
    path += L"SaoAuto.UserGuide.WebView2";
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    const std::wstring probe = path + L"\\.write-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    CloseHandle(file);
    return path;
}

void cancel_mouse(GuideState& state) noexcept {
    if (!state.composition) return;
    const uint32_t masks[]{MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2};
    const COREWEBVIEW2_MOUSE_EVENT_KIND kinds[]{COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP,
        COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP, COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP,
        COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP};
    for (size_t i = 0; i < _countof(masks); ++i) {
        if ((state.mouse_buttons & masks[i]) == 0) continue;
        state.mouse_buttons &= ~masks[i];
        (void)state.composition->SendMouseInput(kinds[i],
            static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(state.mouse_buttons),
            i < 3 ? 0u : (i == 3 ? XBUTTON1 : XBUTTON2), state.mouse);
    }
    (void)state.composition->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
        COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, POINT{});
}

void apply_visibility(GuideState& state) noexcept {
    const bool shown = state.visible && state.ready && !state.failed && !state.closing &&
                       state.target_generation != 0;
    if (!shown) cancel_mouse(state);
    sao_status_t status = SAO_STATUS_OK;
    if (state.slot) {
        status = sao_ui_composition_slot_set_input_policy(state.slot, shown, true);
        if (status == SAO_STATUS_OK) status = sao_ui_composition_slot_set_visible(state.slot, shown);
    }
    const HRESULT visible_status = state.controller ? state.controller->put_IsVisible(shown ? TRUE : FALSE) : S_OK;
    if (shown && status == SAO_STATUS_OK && SUCCEEDED(visible_status)) state.presented = true;
    if (state.status_panel)
        (void)sao_ui_panel_set_visible(state.status_panel, state.visible && !state.ready && !state.closing);
    if (!shown && state.sound_group) (void)sao_ui_sound_group_stop(state.sound_group);
}

void status_body(GuideState& state, bool failure) {
    const json document{{"version", 1}, {"title", ""}, {"nodes", json::array({
        {{"type", "section"}, {"title", failure ? "指南加载失败" : "用户指南"},
         {"children", json::array({
            {{"type", "text"}, {"text", failure
                ? "WebView2 页面尚未就绪。检查运行时及安装目录中的指南资源，然后重试。"
                : "正在载入离线手册，页面将在当前界面中显示。"}, {"wrap", true}, {"height", 64}},
            {{"type", "row"}, {"children", json::array({
                {{"type", "button"}, {"id", "guide-retry"}, {"label", "重新加载"},
                 {"action", "guide.retry"}, {"disabled", !failure}, {"style", "primary"}, {"height", 36}},
                {{"type", "button"}, {"id", "guide-close"}, {"label", "关闭指南"},
                 {"action", "guide.close"}, {"height", 36}}})}}})}}
    })}};
    const std::string text = document.dump();
    (void)sao_ui_panel_set_spec(state.status_panel,
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void fail(GuideState& state, const std::source_location location = std::source_location::current()) noexcept {
    if (state.closing || state.failed) return;
    state.failed = true;
    std::fprintf(stderr, "Guide composition failed at %s:%u\n", location.function_name(), location.line());
    state.ready = false;
    try { status_body(state, true); } catch (...) { OutputDebugStringW(L"Guide error UI failed\n"); }
    apply_visibility(state);
}

void post_sound_policy(GuideState& state) noexcept {
    if (!state.view || state.closing) return;
    bool enabled = true;
    int32_t volume = 70;
    (void)sao_ui_sound_get_enabled(&enabled);
    (void)sao_ui_sound_get_volume(&volume);
    try {
        const std::wstring policy = std::wstring(L"{\"type\":\"sao-guide-sfx-policy\",\"enabled\":") +
            (enabled ? L"true" : L"false") + L",\"volume\":" +
            std::to_wstring(std::clamp(volume, 0, 100)) + L"}";
        (void)state.view->PostWebMessageAsJson(policy.c_str());
    } catch (...) {}
}

void route_sound(GuideState& state, std::wstring_view message) noexcept {
    if (message == L"sao-guide-sfx-stop") {
        if (state.sound_group) (void)sao_ui_sound_group_stop(state.sound_group);
        return;
    }
    constexpr std::wstring_view prefix = L"sao-guide-sfx-play:";
    if (!state.visible || !message.starts_with(prefix) || message.size() > 64) return;
    message.remove_prefix(prefix.size());
    const size_t delimiter = message.find(L':');
    if (delimiter == std::wstring_view::npos) return;
    const auto name = message.substr(0, delimiter), gain = message.substr(delimiter + 1);
    if (gain.empty() || gain.size() > 3) return;
    int volume = 0;
    for (wchar_t value : gain) {
        if (value < L'0' || value > L'9') return;
        volume = volume * 10 + value - L'0';
    }
    if (volume > 100) return;
    SaoUiSoundCue cue = SAO_UI_SOUND_COUNT;
    if (name == L"click") cue = SAO_UI_SOUND_CLICK;
    else if (name == L"menu_open") cue = SAO_UI_SOUND_MENU_OPEN;
    else if (name == L"menu_close") cue = SAO_UI_SOUND_MENU_CLOSE;
    else if (name == L"submenu") cue = SAO_UI_SOUND_SUBMENU;
    else if (name == L"panel") cue = SAO_UI_SOUND_PANEL;
    else if (name == L"alert_close") cue = SAO_UI_SOUND_ALERT_CLOSE;
    if (cue == SAO_UI_SOUND_COUNT) return;
    if (!state.sound_group && sao_ui_sound_group_create(&state.sound_group) != SAO_STATUS_OK) return;
    (void)sao_ui_sound_play_in_group(cue, volume, state.sound_group);
}

void SAO_UI_CALL mouse_input(uint32_t message, uint32_t keys, float x, float y,
                            int32_t button, int32_t wheel, void* data) {
    auto& state = *static_cast<GuideState*>(data);
    if (state.owner != GetCurrentThreadId() || !state.composition || state.closing) return;
    CallbackScope scope(state);
    if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED) { cancel_mouse(state); return; }
    if (!state.ready || !state.visible || state.failed || !std::isfinite(x) || !std::isfinite(y) ||
        x < -1000000 || x > 1000000 || y < -1000000 || y > 1000000) return;
    const bool down = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
        message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK ||
        message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK ||
        message == WM_XBUTTONDOWN || message == WM_XBUTTONDBLCLK;
    const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
    const bool x_button = message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK;
    if ((!down && !up && message != WM_MOUSEMOVE && message != WM_MOUSELEAVE &&
         message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) ||
        (x_button && button != 3 && button != 4)) return;
    if (down) {
        (void)SetFocus(state.parent);
        (void)state.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    state.mouse = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
    const bool leaving = message == WM_MOUSELEAVE;
    const uint32_t mask = button == 0 ? MK_LBUTTON : button == 1 ? MK_RBUTTON :
        button == 2 ? MK_MBUTTON : button == 3 ? MK_XBUTTON1 : button == 4 ? MK_XBUTTON2 : 0;
    if (down) state.mouse_buttons |= mask;
    if (up) state.mouse_buttons &= ~mask;
    const UINT32 extra = x_button ? (button == 3 ? XBUTTON1 : XBUTTON2) :
        (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ? static_cast<UINT32>(wheel) : 0;
    const HRESULT status = state.composition->SendMouseInput(
        static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(message),
        static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(leaving ? 0 : keys),
        extra, leaving ? POINT{} : state.mouse);
    if (FAILED(status)) fail(state);
}

void SAO_UI_CALL status_action(const char* action, const uint8_t*, size_t, void* data) {
    auto& state = *static_cast<GuideState*>(data);
    CallbackScope scope(state);
    if (state.closing || !action) return;
    if (std::string_view(action) == "guide.retry") state.retry_requested = true;
    else if (std::string_view(action) == "guide.close") state.visible = false;
    apply_visibility(state);
}

void SAO_UI_CALL status_event(int32_t event, void* data) {
    auto& state = *static_cast<GuideState*>(data);
    if (event == SAO_UI_PANEL_EVENT_CLOSE) {
        CallbackScope scope(state);
        state.visible = false;
        apply_visibility(state);
    }
}

sao_status_t sync_target(GuideState& state) noexcept {
    SaoOverlayHostClientRect client{};
    sao_status_t status = sao_ui_overlay_host_get_client_rect(sao_ui_compositor_host(state.compositor), &client);
    if (status != SAO_STATUS_OK) return status;
    const int width = std::max(1, client.width), height = std::max(1, client.height);
    if (width != state.width || height != state.height) {
        status = sao_ui_composition_slot_set_geometry(state.slot, 0, 0, width, height);
        if (status != SAO_STATUS_OK) return status;
        state.width = width; state.height = height;
        if (state.controller && FAILED(state.controller->put_Bounds(RECT{0, 0, width, height}))) return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (state.controller && (state.host_x != client.x || state.host_y != client.y))
        (void)state.controller->NotifyParentWindowPositionChanged();
    state.host_x = client.x; state.host_y = client.y;
    uint32_t dpi = 96;
    (void)sao_ui_compositor_host_dpi(state.compositor, &dpi, nullptr);
    if (state.controller3 && state.dpi != dpi &&
        FAILED(state.controller3->put_RasterizationScale(static_cast<double>(dpi) / 96.0))) return SAO_STATUS_ERR_OS_CALL_FAILED;
    state.dpi = dpi;
    if (!state.composition) return SAO_STATUS_OK;
    SaoUiCompositionTarget target{};
    target.struct_size = sizeof(target);
    status = sao_ui_composition_slot_get_target(state.slot, &target);
    if (status != SAO_STATUS_OK) return status;
    if (target.target_generation != state.target_generation || !target.root_visual_target) {
        if (FAILED(state.composition->put_RootVisualTarget(static_cast<IUnknown*>(target.root_visual_target))))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.target_generation = target.root_visual_target ? target.target_generation : 0;
        status = sao_ui_composition_slot_commit(state.slot);
        if (status != SAO_STATUS_OK && !transient(status)) return status;
        apply_visibility(state);
    }
    return SAO_STATUS_OK;
}

HRESULT setup_controller(const std::shared_ptr<GuideState>& state, ICoreWebView2CompositionController* composition) {
    state->composition = composition;
    HRESULT hr = composition->QueryInterface(IID_PPV_ARGS(&state->controller));
    if (FAILED(hr)) return hr;
    hr = state->controller.As(&state->controller3);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = state->controller3->put_ShouldDetectMonitorScaleChanges(FALSE)) ||
        FAILED(hr = state->controller3->put_RasterizationScale(state->dpi / 96.0)) ||
        FAILED(hr = state->controller->put_Bounds(RECT{0, 0, state->width, state->height})) ||
        FAILED(hr = state->controller->put_IsVisible(FALSE)) ||
        FAILED(hr = state->controller->get_CoreWebView2(&state->view))) return hr;
    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(hr = state->view->get_Settings(&settings))) return hr;
    (void)settings->put_IsWebMessageEnabled(TRUE);
    (void)settings->put_AreDefaultContextMenusEnabled(FALSE);
    (void)settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    (void)settings->put_AreDevToolsEnabled(FALSE);
    (void)settings->put_IsStatusBarEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) (void)settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    auto starting = Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            CallbackScope scope(*state);
            LPWSTR uri = nullptr;
            const bool allowed = !state->closing && SUCCEEDED(args->get_Uri(&uri)) && same_document(*state, uri);
            CoTaskMemFree(uri);
            if (!allowed) (void)args->put_Cancel(TRUE);
            else { state->ready = false; apply_visibility(*state); }
            return S_OK;
        });
    if (FAILED(hr = state->view->add_NavigationStarting(starting.Get(), &state->navigation_starting))) return hr;
    state->navigation_starting_set = true;
    auto completed = Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            CallbackScope scope(*state);
            if (state->closing || state->failed) return S_OK;
            BOOL success = FALSE;
            if (FAILED(args->get_IsSuccess(&success)) || !success) { fail(*state); return S_OK; }
            state->ready = true;
            apply_visibility(*state);
#ifndef NDEBUG
            std::fprintf(stderr, "USER_GUIDE_HTML_READY compositor=%d\n", state->presented ? 1 : 0);
            std::fflush(stderr);
#endif
            post_sound_policy(*state);
            return S_OK;
        });
    if (FAILED(hr = state->view->add_NavigationCompleted(completed.Get(), &state->navigation_completed))) return hr;
    state->navigation_completed_set = true;
    auto messages = Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            CallbackScope scope(*state);
            if (state->closing) return S_OK;
            LPWSTR source = nullptr, message = nullptr;
            const bool allowed = SUCCEEDED(args->get_Source(&source)) && same_document(*state, source);
            const HRESULT result = args->TryGetWebMessageAsString(&message);
            if (allowed && SUCCEEDED(result) && message) {
                const std::wstring_view value(message);
                if (value == L"sao-guide-close") { state->visible = false; apply_visibility(*state); }
                else if (value == L"sao-guide-sfx-ready" || value == L"sao-guide-sfx-refresh") post_sound_policy(*state);
                else route_sound(*state, value);
            }
            CoTaskMemFree(source); CoTaskMemFree(message);
            return S_OK;
        });
    if (FAILED(hr = state->view->add_WebMessageReceived(messages.Get(), &state->web_message))) return hr;
    state->web_message_set = true;
    auto cursor = Microsoft::WRL::Callback<ICoreWebView2CursorChangedEventHandler>(
        [state](ICoreWebView2CompositionController* sender, IUnknown*) -> HRESULT {
            CallbackScope scope(*state);
            HCURSOR value = nullptr;
            if (state->visible && !state->closing && SUCCEEDED(sender->get_Cursor(&value))) SetCursor(value);
            return S_OK;
        });
    if (FAILED(hr = composition->add_CursorChanged(cursor.Get(), &state->cursor_changed))) return hr;
    state->cursor_changed_set = true;
    auto crashed = Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            CallbackScope scope(*state); fail(*state); return S_OK;
        });
    if (FAILED(hr = state->view->add_ProcessFailed(crashed.Get(), &state->process_failed))) return hr;
    state->process_failed_set = true;
    auto popup = Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); });
    if (FAILED(hr = state->view->add_NewWindowRequested(popup.Get(), &state->new_window))) return hr;
    state->new_window_set = true;
    const sao_status_t target_status = sync_target(*state);
    if (target_status != SAO_STATUS_OK && !transient(target_status)) return E_FAIL;
    const std::wstring url = state->url + (state->native_intro_completed ? L"#sao-native-intro-complete" : L"");
    return state->view->Navigate(url.c_str());
}

HRESULT start_environment(const std::shared_ptr<GuideState>& state, const std::wstring& profile) {
    auto handler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            CallbackScope scope(*state);
            --state->pending_async;
            if (state->closing || state->failed) return S_OK;
            try {
                if (FAILED(result) || !environment) {
                    if (!state->profile_retried && !state->retry_profile.empty()) {
                        state->profile_retried = true;
                        if (SUCCEEDED(start_environment(state, state->retry_profile))) return S_OK;
                    }
                    fail(*state); return S_OK;
                }
                state->environment = environment;
                ComPtr<ICoreWebView2Environment3> environment3;
                if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&environment3)))) { fail(*state); return S_OK; }
                auto controller_handler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [state](HRESULT controller_result, ICoreWebView2CompositionController* controller) -> HRESULT {
                        CallbackScope controller_scope(*state);
                        --state->pending_async;
                        if (state->closing || state->failed) {
                            ComPtr<ICoreWebView2Controller> base;
                            if (controller && SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&base)))) (void)base->Close();
                            return S_OK;
                        }
                        try {
                            if (FAILED(controller_result) || !controller || FAILED(setup_controller(state, controller))) fail(*state);
                        } catch (...) { fail(*state); }
                        return S_OK;
                    });
                if (!controller_handler) { fail(*state); return S_OK; }
                ++state->pending_async;
                if (FAILED(environment3->CreateCoreWebView2CompositionController(state->parent, controller_handler.Get()))) {
                    --state->pending_async; fail(*state);
                }
            } catch (...) { fail(*state); }
            return S_OK;
        });
    if (!handler) return E_OUTOFMEMORY;
    ++state->pending_async;
    const HRESULT status = state->create_environment(nullptr, profile.c_str(), nullptr, handler.Get());
    if (FAILED(status)) --state->pending_async;
    return status;
}

bool destroy_state(const std::shared_ptr<GuideState>& state) noexcept {
    if (state->owner != GetCurrentThreadId() || state->callback_depth) return false;
    state->closing = true;
    apply_visibility(*state);
    if (state->pending_async) return false;
    if (state->view) {
        if (state->navigation_starting_set) (void)state->view->remove_NavigationStarting(state->navigation_starting);
        if (state->navigation_completed_set) (void)state->view->remove_NavigationCompleted(state->navigation_completed);
        if (state->web_message_set) (void)state->view->remove_WebMessageReceived(state->web_message);
        if (state->process_failed_set) (void)state->view->remove_ProcessFailed(state->process_failed);
        if (state->new_window_set) (void)state->view->remove_NewWindowRequested(state->new_window);
        state->navigation_starting_set = state->navigation_completed_set = state->web_message_set = false;
        state->process_failed_set = state->new_window_set = false;
    }
    if (state->composition) {
        if (state->cursor_changed_set) (void)state->composition->remove_CursorChanged(state->cursor_changed);
        state->cursor_changed_set = false;
        (void)state->composition->put_RootVisualTarget(nullptr);
        if (state->slot) (void)sao_ui_composition_slot_commit(state->slot);
    }
    if (state->controller) (void)state->controller->Close();
    state->view.Reset(); state->controller3.Reset(); state->controller.Reset();
    state->composition.Reset(); state->environment.Reset();
    if (state->slot) {
        if (sao_ui_composition_slot_set_mouse_handler(state->slot, nullptr, nullptr) != SAO_STATUS_OK ||
            sao_ui_composition_slot_try_destroy(state->slot) != SAO_STATUS_OK) return false;
        state->slot = nullptr;
    }
    if (state->status_panel) {
        (void)sao_ui_panel_set_action_handler(state->status_panel, nullptr, nullptr);
        (void)sao_ui_panel_set_event_handler(state->status_panel, nullptr, nullptr);
        sao_ui_panel_destroy(state->status_panel);
        state->status_panel = nullptr;
    }
    return true;
}
} // namespace
#endif

namespace sao::launcher {
bool openUserGuideInWebView(const wchar_t* path) noexcept { return openUserGuideInWebView(path, false); }

bool openUserGuideInWebView(const wchar_t* path, bool native_intro_completed) noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    if (!path || !*path) return false;
    try {
        void* raw = nullptr;
        if (sao_sdk_platform_get_ui_compositor(&raw) != SAO_SDK_OK || !raw) return false;
        const auto compositor = static_cast<sao_ui_compositor_handle_t>(raw);
        if (sao_ui_compositor_require_owner_thread(compositor) != SAO_STATUS_OK) return false;
        auto state = g_guide.load();
        if (state) {
            if (state->owner != GetCurrentThreadId() || state->compositor != compositor) return false;
            if (!state->failed && !state->closing && state->path == path) {
                state->visible = true;
                if (native_intro_completed) {
                    state->native_intro_completed = true;
                    if (state->view) (void)state->view->ExecuteScript(L"window.location.hash='sao-native-intro-complete';", nullptr);
                }
                apply_visibility(*state);
                return true;
            }
            if (!destroy_state(state)) return false;
            g_guide.store(nullptr);
            state.reset();
        }
        state = std::make_shared<GuideState>();
        state->compositor = compositor;
        state->parent = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        state->owner = GetCurrentThreadId();
        state->path = path; state->url = file_uri(path);
        state->native_intro_completed = native_intro_completed;
        state->startup_deadline = GetTickCount64() + 15000;
        if (!state->parent) return false;
        SaoOverlayHostClientRect client{};
        if (sao_ui_overlay_host_get_client_rect(sao_ui_compositor_host(compositor), &client) != SAO_STATUS_OK) return false;
        SaoPanelConfig panel{};
        panel.panel_id_utf8 = "sao.user-guide.status"; panel.title_utf8 = "用户指南";
        panel.default_width = std::min(620, std::max(400, client.width)); panel.default_height = 260;
        panel.default_x = std::max(0, (client.width - panel.default_width) / 2);
        panel.default_y = std::max(0, (client.height - panel.default_height) / 2);
        panel.min_width = 360; panel.min_height = 240;
        panel.movable = true; panel.resizable = true; panel.show_titlebar = true;
        panel.show_close_button = true; panel.single_instance = true;
        if (sao_ui_panel_create(compositor, &panel, &state->status_panel) != SAO_STATUS_OK) return false;
        g_guide.store(state);
        (void)sao_ui_panel_set_action_handler(state->status_panel, status_action, state.get());
        (void)sao_ui_panel_set_event_handler(state->status_panel, status_event, state.get());
        status_body(*state, false);
        apply_visibility(*state);
        SaoUiCompositionSlotConfig slot{};
        slot.struct_size = sizeof(slot); slot.name_utf8 = "sao.user-guide";
        slot.width = std::max(1, client.width); slot.height = std::max(1, client.height);
        slot.z_order = 2000; slot.band = SAO_UI_COMPOSITION_BAND_BELOW_NATIVE;
        slot.opacity = 1.0F; slot.focusable = true;
        if (sao_ui_composition_slot_create(compositor, &slot, &state->slot) != SAO_STATUS_OK ||
            sao_ui_composition_slot_set_mouse_handler(state->slot, mouse_input, state.get()) != SAO_STATUS_OK) {
            fail(*state); return true;
        }
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { fail(*state); return true; }
        state->com_initialized = true;
        wchar_t module[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, _countof(module));
        if (!length || length >= _countof(module)) { fail(*state); return true; }
        std::wstring loader_path(module, length);
        loader_path.erase(loader_path.find_last_of(L"\\/") + 1);
        loader_path += L"WebView2Loader.dll";
        state->loader = LoadLibraryW(loader_path.c_str());
        if (!state->loader) { fail(*state); return true; }
        state->create_environment = reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(state->loader, "CreateCoreWebView2EnvironmentWithOptions"));
        if (!state->create_environment) { fail(*state); return true; }
        std::wstring profile = profile_path(false);
        state->retry_profile = profile_path(true);
        if (profile.empty()) { profile.swap(state->retry_profile); state->profile_retried = true; }
        if (profile.empty()) { fail(*state); return true; }
        const sao_status_t target_status = sync_target(*state);
        if (target_status != SAO_STATUS_OK && !transient(target_status)) { fail(*state); return true; }
        HRESULT started = start_environment(state, profile);
        if (FAILED(started) && !state->profile_retried && !state->retry_profile.empty()) {
            state->profile_retried = true;
            started = start_environment(state, state->retry_profile);
        }
        if (FAILED(started)) fail(*state);
        return true;
    } catch (...) {
        if (auto state = g_guide.load(); state && state->owner == GetCurrentThreadId()) fail(*state);
        return false;
    }
#else
    (void)path; (void)native_intro_completed;
    return false;
#endif
}

void tickUserGuideWebView() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    auto state = g_guide.load();
    if (!state || state->owner != GetCurrentThreadId()) return;
    if (state->retry_requested) {
        try {
            const auto path = state->path;
            if (openUserGuideInWebView(path.c_str(), state->native_intro_completed)) state->retry_requested = false;
        } catch (...) { fail(*state); }
        return;
    }
    if (state->closing || state->failed) return;
    if (!state->ready && GetTickCount64() >= state->startup_deadline) { fail(*state); return; }
    const sao_status_t status = sync_target(*state);
    if (status != SAO_STATUS_OK && !transient(status)) fail(*state);
#endif
}

void hideUserGuideWebView() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    const auto state = g_guide.load();
    if (state && state->owner == GetCurrentThreadId()) {
        state->visible = false;
        apply_visibility(*state);
    }
#endif
}

bool userGuideWebViewReady() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    const auto state = g_guide.load();
    return state && state->owner == GetCurrentThreadId() && state->presented;
#else
    return false;
#endif
}

bool shutdownUserGuideWebView() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    const auto state = g_guide.load();
    if (!state) return true;
    if (!destroy_state(state)) return false;
    g_guide.store(nullptr);
#endif
    return true;
}

void refreshUserGuideSoundPolicy() noexcept {
#if defined(SAO_LAUNCHER_HAS_WEBVIEW2) && defined(SAO_LAUNCHER_PLATFORM_COMPOSITION_PROVIDER)
    const auto state = g_guide.load();
    if (state && state->owner == GetCurrentThreadId()) post_sound_policy(*state);
#endif
}
} // namespace sao::launcher
