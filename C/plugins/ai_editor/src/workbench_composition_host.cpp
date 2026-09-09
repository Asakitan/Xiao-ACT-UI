#include "workbench_composition_host.h"

#if defined(SAO_AI_EDITOR_HAS_WEBVIEW) && SAO_AI_EDITOR_HAS_WEBVIEW

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <combaseapi.h>
#include <objbase.h>

#include <WebView2.h>
#include <wrl/async.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace sao::ai_editor::workbench {
namespace {

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

using CreateEnvironmentFn = HRESULT(WINAPI*)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

constexpr wchar_t kVirtualHost[] = L"sao-workbench.local";
constexpr wchar_t kWorkbenchUrl[] = L"https://sao-workbench.local/ai_editor_app.html";
constexpr char kChannel[] = "sao.workbench";
constexpr size_t kMaximumMessageBytes = 1024u * 1024u;
constexpr int32_t kSlotZOrder = 1000;
constexpr std::chrono::seconds kHelloTimeout{4};

bool transient_composition_status(sao_status_t status) noexcept {
    return status == SAO_STATUS_ERR_DEVICE_LOST || status == SAO_STATUS_ERR_NOT_INITIALIZED;
}

constexpr uint32_t kMouseMove = WM_MOUSEMOVE;
constexpr uint32_t kMouseLeave = WM_MOUSELEAVE;
constexpr uint32_t kMouseWheel = WM_MOUSEWHEEL;
constexpr uint32_t kLeftButtonDown = WM_LBUTTONDOWN;
constexpr uint32_t kLeftButtonUp = WM_LBUTTONUP;
constexpr uint32_t kLeftButtonDoubleClick = WM_LBUTTONDBLCLK;
constexpr uint32_t kRightButtonDown = WM_RBUTTONDOWN;
constexpr uint32_t kRightButtonUp = WM_RBUTTONUP;
constexpr uint32_t kRightButtonDoubleClick = WM_RBUTTONDBLCLK;
constexpr uint32_t kMiddleButtonDown = WM_MBUTTONDOWN;
constexpr uint32_t kMiddleButtonUp = WM_MBUTTONUP;
constexpr uint32_t kMiddleButtonDoubleClick = WM_MBUTTONDBLCLK;
constexpr uint32_t kXButtonDown = WM_XBUTTONDOWN;
constexpr uint32_t kXButtonUp = WM_XBUTTONUP;
constexpr uint32_t kXButtonDoubleClick = WM_XBUTTONDBLCLK;
constexpr uint32_t kCaptureChanged = WM_CAPTURECHANGED;
constexpr uint32_t kCancelMode = WM_CANCELMODE;

bool utf8_to_wide(std::string_view input, std::wstring* output) {
    if (output == nullptr)
        return false;
    output->clear();
    if (input.empty())
        return true;
    if (input.size() > static_cast<size_t>(INT_MAX))
        return false;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                             static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0)
        return false;
    output->assign(static_cast<size_t>(required), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), output->data(), required) == required;
}

bool wide_to_utf8(std::wstring_view input, std::string* output) {
    if (output == nullptr)
        return false;
    output->clear();
    if (input.empty())
        return true;
    if (input.size() > static_cast<size_t>(INT_MAX))
        return false;
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                             static_cast<int>(input.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (required <= 0)
        return false;
    output->assign(static_cast<size_t>(required), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), output->data(), required, nullptr,
                               nullptr) == required;
}

std::wstring without_fragment(std::wstring_view value) {
    const size_t fragment = value.find(L'#');
    return std::wstring(value.substr(0, fragment));
}

bool is_workbench_document(std::wstring_view value) {
    return without_fragment(value) == kWorkbenchUrl;
}

bool make_navigation_challenge(std::string* output) {
    if (output == nullptr)
        return false;
    GUID value{};
    if (FAILED(CoCreateGuid(&value)))
        return false;
    std::array<wchar_t, 40> buffer{};
    const int length = StringFromGUID2(value, buffer.data(), static_cast<int>(buffer.size()));
    return length > 1 &&
           wide_to_utf8(std::wstring_view(buffer.data(), static_cast<size_t>(length - 1)), output);
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

void module_anchor() {}

std::filesystem::path module_directory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&module_anchor), &module) ||
        module == nullptr) {
        return {};
    }
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(module, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::filesystem::path workbench_asset_root(const std::filesystem::path& module_dir) {
    const auto root = module_dir / L"assets" / L"ai_editor" / L"workbench";
    if (!regular_file(root / L"ai_editor_app.html") ||
        !regular_file(root / L"native-bridge.js") ||
        !regular_file(root / L"classic-theme.css")) {
        return {};
    }
    return root;
}

std::filesystem::path user_data_folder() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    std::filesystem::path folder(std::wstring(buffer.data(), length));
    folder /= L"SaoAuto";
    folder /= L"WebView2";
    folder /= L"AIWorkbench";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return error ? std::filesystem::path{} : folder;
}

struct HostState {
    enum class Phase : uint32_t {
        starting,
        controller_ready,
        ready,
        failed,
        closing,
    };

    sao_ui_compositor_handle_t compositor{};
    sao_ai_editor_launcher_t launcher{};
    sao_ui_composition_slot_handle_t slot{};
    HWND parent_window{};
    DWORD owner_thread{};
    HMODULE loader{};
    CreateEnvironmentFn create_environment{};
    bool com_initialized{};
    bool requested_visible{};
    bool close_requested{};
    bool handshake_complete{};
    bool navigation_started{};
    bool navigation_completed{};
    bool process_failed{};
    uint32_t pending_async{};
    uint32_t callback_depth{};
    uint32_t mouse_buttons{};
    int32_t mouse_x{};
    int32_t mouse_y{};
    int32_t host_x{};
    int32_t host_y{};
    int32_t width{1};
    int32_t height{1};
    uint32_t dpi{96};
    uint64_t bound_target_generation{};
    uint64_t navigation_generation{};
    uint64_t handshake_generation{};
    uint64_t current_navigation_id{};
    std::chrono::steady_clock::time_point hello_deadline{};
    Phase phase{Phase::starting};
    sao_status_t failure_status{SAO_STATUS_OK};
    sao_status_t input_failure_status{SAO_STATUS_OK};
    std::string handshake_challenge;
    std::filesystem::path asset_root;
    std::filesystem::path profile_root;
    ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> environment_handler;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Environment3> environment3;
    ComPtr<ICoreWebView2CompositionController> composition_controller;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2Controller4> controller4;
    ComPtr<ICoreWebView2> view;
    EventRegistrationToken navigation_starting_token{};
    EventRegistrationToken navigation_completed_token{};
    EventRegistrationToken web_message_token{};
    EventRegistrationToken cursor_changed_token{};
    EventRegistrationToken process_failed_token{};
    bool navigation_starting_registered{};
    bool navigation_completed_registered{};
    bool web_message_registered{};
    bool cursor_changed_registered{};
    bool process_failed_registered{};

    ~HostState() {
        if (owner_thread == GetCurrentThreadId()) {
            if (composition_controller) {
                (void)composition_controller->put_RootVisualTarget(nullptr);
                if (slot != nullptr)
                    (void)sao_ui_composition_slot_commit(slot);
            }
            if (controller)
                (void)controller->Close();
            if (slot != nullptr) {
                (void)sao_ui_composition_slot_set_mouse_handler(slot, nullptr, nullptr);
                (void)sao_ui_composition_slot_try_destroy(slot);
                slot = nullptr;
            }
        }
        if (loader != nullptr) {
            FreeLibrary(loader);
            loader = nullptr;
        }
        if (com_initialized && owner_thread == GetCurrentThreadId()) {
            CoUninitialize();
            com_initialized = false;
        }
    }
};

struct CallbackScope {
    explicit CallbackScope(const std::shared_ptr<HostState>& state) : state_(state) {
        ++state_->callback_depth;
    }
    ~CallbackScope() { --state_->callback_depth; }
    std::shared_ptr<HostState> state_;
};

void fail_state(HostState& state, sao_status_t status) noexcept {
    if (state.phase == HostState::Phase::closing || state.phase == HostState::Phase::failed)
        return;
    state.failure_status = status == SAO_STATUS_OK ? SAO_STATUS_ERR_OS_CALL_FAILED : status;
#ifndef NDEBUG
    std::fprintf(stderr, "AI Workbench composition host async failure: %d\n",
                 state.failure_status);
#endif
    state.phase = HostState::Phase::failed;
    state.requested_visible = false;
    if (state.slot != nullptr) {
        (void)sao_ui_composition_slot_set_input_policy(state.slot, false, true);
        (void)sao_ui_composition_slot_set_visible(state.slot, false);
    }
    if (state.controller)
        (void)state.controller->put_IsVisible(FALSE);
}

void fail_state(const std::shared_ptr<HostState>& state, sao_status_t status) noexcept {
    fail_state(*state, status);
}

sao_status_t post_json(HostState& state, const json& message) noexcept {
    if (!state.view)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    try {
        const std::string payload = message.dump();
        if (payload.size() > kMaximumMessageBytes)
            return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
        std::wstring wide;
        if (!utf8_to_wide(payload, &wide))
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        return SUCCEEDED(state.view->PostWebMessageAsJson(wide.c_str()))
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

json error_reply(const std::string& challenge, std::string id, std::string code,
                 std::string message) {
    return {{"channel", kChannel},
            {"kind", "reply"},
            {"challenge", challenge},
            {"id", std::move(id)},
            {"ok", false},
            {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}

sao_status_t apply_requested_visibility(HostState& state) noexcept {
    if (state.slot == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    const bool can_show = state.requested_visible && state.controller &&
                          state.navigation_completed && state.handshake_complete &&
                          state.navigation_generation != 0 &&
                          state.handshake_generation == state.navigation_generation &&
                          state.phase == HostState::Phase::ready;
    sao_status_t status = sao_ui_composition_slot_set_input_policy(
        state.slot, can_show && state.bound_target_generation != 0, true);
    if (status == SAO_STATUS_OK)
        status = sao_ui_composition_slot_set_visible(
            state.slot, can_show && state.bound_target_generation != 0);
    if (state.controller) {
        const HRESULT visible_status = state.controller->put_IsVisible(can_show ? TRUE : FALSE);
        if (status == SAO_STATUS_OK && FAILED(visible_status))
            status = SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return status;
}

sao_status_t bind_current_target(HostState& state) noexcept {
    if (!state.composition_controller || state.slot == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    SaoUiCompositionTarget target{};
    target.struct_size = sizeof(target);
    const sao_status_t target_status = sao_ui_composition_slot_get_target(state.slot, &target);
    if (target_status != SAO_STATUS_OK)
        return target_status;
    if (target.target_generation == state.bound_target_generation &&
        target.root_visual_target != nullptr) {
        return SAO_STATUS_OK;
    }
    const HRESULT bind_status = state.composition_controller->put_RootVisualTarget(
        static_cast<IUnknown*>(target.root_visual_target));
    if (FAILED(bind_status))
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    if (target.root_visual_target != nullptr) {
        const sao_status_t commit_status = sao_ui_composition_slot_commit(state.slot);
        if (commit_status != SAO_STATUS_OK)
            return commit_status;
    }
    state.bound_target_generation = target.target_generation;
    if (target.root_visual_target == nullptr) {
        sao_status_t hide_status =
            sao_ui_composition_slot_set_input_policy(state.slot, false, true);
        if (hide_status == SAO_STATUS_OK)
            hide_status = sao_ui_composition_slot_set_visible(state.slot, false);
        if (hide_status != SAO_STATUS_OK && !transient_composition_status(hide_status))
            return hide_status;
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    return apply_requested_visibility(state);
}

sao_status_t update_bounds_and_dpi(HostState& state) noexcept {
    const auto host = sao_ui_compositor_host(state.compositor);
    if (host == nullptr)
        return SAO_STATUS_ERR_NOT_INITIALIZED;
    SaoOverlayHostClientRect client{};
    sao_status_t status = sao_ui_overlay_host_get_client_rect(host, &client);
    if (status != SAO_STATUS_OK)
        return status;
    const int32_t width = std::max(1, client.width);
    const int32_t height = std::max(1, client.height);
    const bool moved = client.x != state.host_x || client.y != state.host_y;
    const bool resized = width != state.width || height != state.height;
    if (resized) {
        status = sao_ui_composition_slot_set_geometry(state.slot, 0, 0, width, height);
        if (status != SAO_STATUS_OK)
            return status;
        state.width = width;
        state.height = height;
        if (state.controller) {
            const RECT bounds{0, 0, width, height};
            if (FAILED(state.controller->put_Bounds(bounds)))
                return SAO_STATUS_ERR_OS_CALL_FAILED;
        }
    }
    state.host_x = client.x;
    state.host_y = client.y;
    uint32_t dpi = 96;
    status = sao_ui_compositor_host_dpi(state.compositor, &dpi, nullptr);
    if (status != SAO_STATUS_OK)
        return status;
    dpi = dpi == 0 ? 96 : dpi;
    if (state.controller4 && dpi != state.dpi) {
        const double scale = static_cast<double>(dpi) / 96.0;
        if (FAILED(state.controller4->put_RasterizationScale(scale)))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.dpi = dpi;
    }
    if (state.controller && (moved || resized) &&
        FAILED(state.controller->NotifyParentWindowPositionChanged()))
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    return SAO_STATUS_OK;
}

bool map_mouse_kind(uint32_t message, COREWEBVIEW2_MOUSE_EVENT_KIND* out_kind) noexcept {
    if (out_kind == nullptr)
        return false;
    switch (message) {
    case kMouseMove: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE; return true;
    case kMouseLeave: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE; return true;
    case kMouseWheel: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL; return true;
    case kLeftButtonDown: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN; return true;
    case kLeftButtonUp: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP; return true;
    case kLeftButtonDoubleClick:
        *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
        return true;
    case kRightButtonDown: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN; return true;
    case kRightButtonUp: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP; return true;
    case kRightButtonDoubleClick:
        *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK;
        return true;
    case kMiddleButtonDown:
        *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
        return true;
    case kMiddleButtonUp: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP; return true;
    case kMiddleButtonDoubleClick:
        *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK;
        return true;
    case kXButtonDown: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN; return true;
    case kXButtonUp: *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP; return true;
    case kXButtonDoubleClick:
        *out_kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK;
        return true;
    default: return false;
    }
}

void note_input_failure(HostState& state, HRESULT status) noexcept {
    if (FAILED(status) && state.input_failure_status == SAO_STATUS_OK)
        state.input_failure_status = SAO_STATUS_ERR_OS_CALL_FAILED;
}

void send_release(HostState& state, uint32_t mask,
                  COREWEBVIEW2_MOUSE_EVENT_KIND kind, UINT32 mouse_data = 0) noexcept {
    if ((state.mouse_buttons & mask) == 0 || !state.composition_controller)
        return;
    const POINT point{state.mouse_x, state.mouse_y};
    const auto remaining = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
        state.mouse_buttons & ~mask);
    const HRESULT status = state.composition_controller->SendMouseInput(
        kind, remaining, mouse_data, point);
    if (SUCCEEDED(status))
        state.mouse_buttons &= ~mask;
    else
        note_input_failure(state, status);
}

void cancel_mouse_state(HostState& state) noexcept {
    send_release(state, MK_LBUTTON, COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP);
    send_release(state, MK_RBUTTON, COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP);
    send_release(state, MK_MBUTTON, COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP);
    send_release(state, MK_XBUTTON1, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP, XBUTTON1);
    send_release(state, MK_XBUTTON2, COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP, XBUTTON2);
    if (state.composition_controller) {
        const POINT point{};
        const HRESULT status = state.composition_controller->SendMouseInput(
            COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
            COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, point);
        note_input_failure(state, status);
    }
}

void SAO_UI_CALL mouse_callback(uint32_t message, uint32_t key_state, float slot_x,
                                float slot_y, int32_t button, int32_t wheel_delta,
                                void* user_data) {
    auto* state = static_cast<HostState*>(user_data);
    if (state == nullptr || !state->composition_controller || !state->requested_visible ||
        state->phase == HostState::Phase::closing || state->phase == HostState::Phase::failed) {
        return;
    }
    state->mouse_x = static_cast<int32_t>(std::lround(slot_x));
    state->mouse_y = static_cast<int32_t>(std::lround(slot_y));
    if (message == kCaptureChanged || message == kCancelMode) {
        cancel_mouse_state(*state);
        return;
    }
    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    if (!map_mouse_kind(message, &kind))
        return;
    const bool focus_message = message == kLeftButtonDown || message == kLeftButtonDoubleClick ||
        message == kRightButtonDown || message == kRightButtonDoubleClick ||
        message == kMiddleButtonDown || message == kMiddleButtonDoubleClick ||
        message == kXButtonDown || message == kXButtonDoubleClick;
    if (focus_message) {
        (void)SetFocus(state->parent_window);
        if (GetFocus() != state->parent_window)
            state->input_failure_status = SAO_STATUS_ERR_OS_CALL_FAILED;
        if (state->controller) {
            note_input_failure(*state, state->controller->MoveFocus(
                                           COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
        }
    }
    const bool leaving = message == kMouseLeave;
    const POINT point = leaving ? POINT{} : POINT{state->mouse_x, state->mouse_y};
    UINT32 mouse_data = 0;
    if (message == kMouseWheel)
        mouse_data = static_cast<UINT32>(wheel_delta);
    else if (message == kXButtonDown || message == kXButtonUp ||
             message == kXButtonDoubleClick)
        mouse_data = button == 3 ? XBUTTON1 : XBUTTON2;
    const HRESULT send_status = state->composition_controller->SendMouseInput(
        kind,
        leaving ? COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE
                : static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(key_state),
        mouse_data, point);
    if (FAILED(send_status)) {
        note_input_failure(*state, send_status);
        return;
    }
    if (message == kLeftButtonDown || message == kLeftButtonDoubleClick)
        state->mouse_buttons |= MK_LBUTTON;
    else if (message == kRightButtonDown || message == kRightButtonDoubleClick)
        state->mouse_buttons |= MK_RBUTTON;
    else if (message == kMiddleButtonDown || message == kMiddleButtonDoubleClick)
        state->mouse_buttons |= MK_MBUTTON;
    else if (message == kXButtonDown || message == kXButtonDoubleClick)
        state->mouse_buttons |= button == 3 ? MK_XBUTTON1 : MK_XBUTTON2;
    else if (message == kLeftButtonUp)
        state->mouse_buttons &= ~MK_LBUTTON;
    else if (message == kRightButtonUp)
        state->mouse_buttons &= ~MK_RBUTTON;
    else if (message == kMiddleButtonUp)
        state->mouse_buttons &= ~MK_MBUTTON;
    else if (message == kXButtonUp)
        state->mouse_buttons &= ~(button == 3 ? MK_XBUTTON1 : MK_XBUTTON2);
    HCURSOR cursor = nullptr;
    if (SUCCEEDED(state->composition_controller->get_Cursor(&cursor)) && cursor != nullptr)
        (void)SetCursor(cursor);
}

sao_status_t handle_message(const std::shared_ptr<HostState>& state,
                            ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    if (args == nullptr || !state->view)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    LPWSTR source_raw = nullptr;
    LPWSTR payload_raw = nullptr;
    const HRESULT source_status = args->get_Source(&source_raw);
    const HRESULT payload_status = args->get_WebMessageAsJson(&payload_raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> source(source_raw, &CoTaskMemFree);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> payload(payload_raw, &CoTaskMemFree);
    if (FAILED(source_status) || FAILED(payload_status) || source_raw == nullptr ||
        payload_raw == nullptr || !is_workbench_document(source_raw)) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    LPWSTR top_raw = nullptr;
    const HRESULT top_status = state->view->get_Source(&top_raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> top(top_raw, &CoTaskMemFree);
    if (FAILED(top_status) || top_raw == nullptr || !is_workbench_document(top_raw) ||
        without_fragment(source_raw) != without_fragment(top_raw)) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    std::string payload_utf8;
    if (!wide_to_utf8(payload_raw, &payload_utf8) || payload_utf8.size() > kMaximumMessageBytes)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    json message;
    try {
        message = json::parse(payload_utf8);
    } catch (...) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (!message.is_object())
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const auto channel_it = message.find("channel");
    const auto kind_it = message.find("kind");
    if (channel_it == message.end() || !channel_it->is_string() ||
        channel_it->get_ref<const std::string&>() != kChannel || kind_it == message.end() ||
        !kind_it->is_string()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::string& kind = kind_it->get_ref<const std::string&>();
    const auto challenge_it = message.find("challenge");
    const bool challenge_matches =
        challenge_it != message.end() && challenge_it->is_string() &&
        challenge_it->get_ref<const std::string&>() == state->handshake_challenge;
    if (kind == "hello") {
        if (!state->navigation_completed || state->navigation_generation == 0 ||
            state->handshake_challenge.empty() || !challenge_matches) {
            return SAO_STATUS_ERR_ACCESS_DENIED;
        }
        state->handshake_complete = true;
        state->handshake_generation = state->navigation_generation;
        state->hello_deadline = {};
        sao_status_t status = post_json(*state,
                                        {{"channel", kChannel},
                                         {"kind", "ready"},
                                         {"challenge", state->handshake_challenge},
                                         {"connected", true},
                                         {"capabilities", json::array({"rpc", "events"})}});
        if (status == SAO_STATUS_OK) {
            state->phase = HostState::Phase::ready;
            status = apply_requested_visibility(*state);
        }
        return transient_composition_status(status) ? SAO_STATUS_OK : status;
    }
    if (kind != "request")
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (!state->navigation_completed || !state->handshake_complete ||
        state->handshake_generation != state->navigation_generation ||
        state->handshake_challenge.empty() || !challenge_matches) {
        return SAO_STATUS_ERR_ACCESS_DENIED;
    }
    const auto id_it = message.find("id");
    const auto method_it = message.find("method");
    const auto args_it = message.find("args");
    if (id_it == message.end() || !id_it->is_string() || method_it == message.end() ||
        !method_it->is_string() || args_it == message.end() || !args_it->is_array()) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const std::string id = id_it->get<std::string>();
    const std::string method = method_it->get<std::string>();
    if (id.empty() || id.size() > 128u || method.empty() || method.size() > 128u ||
        args_it->size() > 64u) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (method == "load_config") {
        return post_json(*state,
                         {{"channel", kChannel}, {"kind", "reply"}, {"id", id},
                          {"challenge", state->handshake_challenge},
                          {"ok", true},
                          {"result", {{"theme", "light"}, {"color_theme", ""}}}});
    }
    if (method == "win_close") {
        state->close_requested = true;
        state->requested_visible = false;
        const sao_status_t hide_status = apply_requested_visibility(*state);
        if (hide_status != SAO_STATUS_OK && !transient_composition_status(hide_status)) {
            fail_state(state, hide_status);
            return post_json(*state,
                            error_reply(state->handshake_challenge, id,
                                     "SAO_WINDOW_CLOSE_FAILED",
                                         "Native workbench close did not converge."));
        }
        return post_json(*state,
                         {{"channel", kChannel}, {"kind", "reply"}, {"id", id},
                         {"challenge", state->handshake_challenge},
                          {"ok", true}, {"result", {{"ok", true}}}});
    }
    return post_json(*state,
                     error_reply(state->handshake_challenge, id, "SAO_METHOD_UNAVAILABLE",
                                 "Native workbench method is not available: " + method));
}

sao_status_t register_view_events(const std::shared_ptr<HostState>& state) noexcept {
    auto navigation_starting = Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            CallbackScope callback(state);
            if (state->phase == HostState::Phase::closing || args == nullptr)
                return S_OK;
            if (state->phase == HostState::Phase::failed) {
                (void)args->put_Cancel(TRUE);
                return S_OK;
            }
            LPWSTR uri_raw = nullptr;
            const HRESULT uri_status = args->get_Uri(&uri_raw);
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> uri(uri_raw, &CoTaskMemFree);
            const bool allowed = SUCCEEDED(uri_status) && uri_raw != nullptr &&
                                 is_workbench_document(uri_raw);
            if (!allowed) {
                (void)args->put_Cancel(TRUE);
                return S_OK;
            }
            UINT64 navigation_id = 0;
            if (FAILED(args->get_NavigationId(&navigation_id)) || navigation_id == 0) {
                (void)args->put_Cancel(TRUE);
                fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
                return S_OK;
            }
            ++state->navigation_generation;
            if (state->navigation_generation == 0)
                ++state->navigation_generation;
            state->current_navigation_id = navigation_id;
            state->navigation_started = true;
            state->navigation_completed = false;
            state->handshake_complete = false;
            state->handshake_generation = 0;
            state->handshake_challenge.clear();
            state->phase = HostState::Phase::controller_ready;
            state->hello_deadline = {};
            const sao_status_t visibility_status = apply_requested_visibility(*state);
            if (visibility_status != SAO_STATUS_OK &&
                !transient_composition_status(visibility_status)) {
                fail_state(state, visibility_status);
                (void)args->put_Cancel(TRUE);
            }
            return S_OK;
        });
    if (!navigation_starting ||
        FAILED(state->view->add_NavigationStarting(navigation_starting.Get(),
                                                    &state->navigation_starting_token))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    state->navigation_starting_registered = true;

    auto navigation_completed =
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [state](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                CallbackScope callback(state);
                if (state->phase == HostState::Phase::closing ||
                    state->phase == HostState::Phase::failed || args == nullptr)
                    return S_OK;
                BOOL success = FALSE;
                UINT64 navigation_id = 0;
                if (FAILED(args->get_IsSuccess(&success)) ||
                    FAILED(args->get_NavigationId(&navigation_id))) {
                    fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
                    return S_OK;
                }
                if (navigation_id != state->current_navigation_id)
                    return S_OK;
                if (success == FALSE) {
                    fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
                    return S_OK;
                }
                state->navigation_completed = true;
                std::string challenge;
                if (!make_navigation_challenge(&challenge)) {
                    fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
                    return S_OK;
                }
                state->handshake_challenge = std::move(challenge);
                const sao_status_t challenge_status =
                    post_json(*state, {{"channel", kChannel},
                                       {"kind", "challenge"},
                                       {"challenge", state->handshake_challenge}});
                if (challenge_status != SAO_STATUS_OK) {
                    fail_state(state, challenge_status);
                    return S_OK;
                }
                state->hello_deadline = std::chrono::steady_clock::now() + kHelloTimeout;
                return S_OK;
            });
    if (!navigation_completed ||
        FAILED(state->view->add_NavigationCompleted(navigation_completed.Get(),
                                                     &state->navigation_completed_token))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    state->navigation_completed_registered = true;

    auto web_message = Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            CallbackScope callback(state);
            if (state->phase == HostState::Phase::closing ||
                state->phase == HostState::Phase::failed)
                return S_OK;
            const sao_status_t status = handle_message(state, args);
            if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_INVALID_ARGUMENT &&
                status != SAO_STATUS_ERR_ACCESS_DENIED) {
                fail_state(state, status);
            }
            return S_OK;
        });
    if (!web_message ||
        FAILED(state->view->add_WebMessageReceived(web_message.Get(),
                                                    &state->web_message_token))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    state->web_message_registered = true;

    auto process_failed = Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
        [state](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            CallbackScope callback(state);
            state->process_failed = true;
            if (state->phase != HostState::Phase::closing)
                fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
            return S_OK;
        });
    if (!process_failed ||
        FAILED(state->view->add_ProcessFailed(process_failed.Get(),
                                               &state->process_failed_token))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    state->process_failed_registered = true;

    auto cursor_changed = Microsoft::WRL::Callback<ICoreWebView2CursorChangedEventHandler>(
        [state](ICoreWebView2CompositionController*, IUnknown*) -> HRESULT {
            CallbackScope callback(state);
            if (state->phase == HostState::Phase::closing || !state->composition_controller)
                return S_OK;
            HCURSOR cursor = nullptr;
            if (SUCCEEDED(state->composition_controller->get_Cursor(&cursor)) && cursor != nullptr)
                (void)SetCursor(cursor);
            return S_OK;
        });
    if (!cursor_changed ||
        FAILED(state->composition_controller->add_CursorChanged(cursor_changed.Get(),
                                                                 &state->cursor_changed_token))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    state->cursor_changed_registered = true;
    return SAO_STATUS_OK;
}

sao_status_t finish_controller_setup(const std::shared_ptr<HostState>& state,
                                     ICoreWebView2CompositionController* composition) noexcept {
    if (composition == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    state->composition_controller = composition;
    if (FAILED(composition->QueryInterface(IID_PPV_ARGS(&state->controller))) ||
        !state->controller) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    if (FAILED(state->controller.As(&state->controller4)) || !state->controller4)
        return SAO_STATUS_ERR_NOT_IMPLEMENTED;
    if (FAILED(state->controller4->put_ShouldDetectMonitorScaleChanges(FALSE)) ||
        FAILED(state->controller4->put_RasterizationScale(
            static_cast<double>(state->dpi) / 96.0))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const RECT bounds{0, 0, state->width, state->height};
    if (FAILED(state->controller->put_Bounds(bounds)) ||
        FAILED(state->controller->get_CoreWebView2(&state->view)) || !state->view) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(state->view->get_Settings(&settings)) || !settings ||
        FAILED(settings->put_IsWebMessageEnabled(TRUE))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    ComPtr<ICoreWebView2_3> view3;
    if (FAILED(state->view.As(&view3)) || !view3 ||
        FAILED(view3->SetVirtualHostNameToFolderMapping(
            kVirtualHost, state->asset_root.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    sao_status_t status = bind_current_target(*state);
    if (status != SAO_STATUS_OK && !transient_composition_status(status))
        return status;
    status = register_view_events(state);
    if (status != SAO_STATUS_OK)
        return status;
    state->phase = HostState::Phase::controller_ready;
    if (FAILED(state->view->Navigate(kWorkbenchUrl)))
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    return apply_requested_visibility(*state);
}

sao_status_t start_environment(const std::shared_ptr<HostState>& state) noexcept {
    state->environment_handler = Microsoft::WRL::Callback<
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            CallbackScope callback(state);
            state->environment_handler.Reset();
            if (state->pending_async != 0)
                --state->pending_async;
            if (state->phase == HostState::Phase::closing)
                return S_OK;
            if (FAILED(result) || environment == nullptr) {
                fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
                return S_OK;
            }
            state->environment = environment;
            if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&state->environment3))) ||
                !state->environment3) {
                fail_state(state, SAO_STATUS_ERR_NOT_IMPLEMENTED);
                return S_OK;
            }
            auto controller_ready = Microsoft::WRL::Callback<
                ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [state](HRESULT controller_result,
                        ICoreWebView2CompositionController* controller) -> HRESULT {
                    CallbackScope controller_callback(state);
                    if (state->pending_async != 0)
                        --state->pending_async;
                    if (state->phase == HostState::Phase::closing) {
                        if (controller != nullptr) {
                            ComPtr<ICoreWebView2Controller> base;
                            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&base))) && base)
                                (void)base->Close();
                        }
                        return S_OK;
                    }
                    if (FAILED(controller_result) || controller == nullptr) {
                        fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
                        return S_OK;
                    }
                    const sao_status_t setup_status = finish_controller_setup(state, controller);
                    if (setup_status != SAO_STATUS_OK)
                        fail_state(state, setup_status);
                    return S_OK;
                });
            if (!controller_ready) {
                fail_state(state, SAO_STATUS_ERR_UNKNOWN);
                return S_OK;
            }
            ++state->pending_async;
            const HRESULT create_status = state->environment3->CreateCoreWebView2CompositionController(
                state->parent_window, controller_ready.Get());
            if (FAILED(create_status)) {
                --state->pending_async;
                fail_state(state, SAO_STATUS_ERR_OS_CALL_FAILED);
            }
            return S_OK;
        });
    if (!state->environment_handler)
        return SAO_STATUS_ERR_UNKNOWN;
    ++state->pending_async;
    const HRESULT create_status = state->create_environment(
        nullptr, state->profile_root.c_str(), nullptr, state->environment_handler.Get());
    if (FAILED(create_status)) {
        --state->pending_async;
        state->environment_handler.Reset();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    return SAO_STATUS_OK;
}

sao_status_t remove_handlers(HostState& state) noexcept {
    if (state.process_failed) {
        state.process_failed_registered = false;
        state.web_message_registered = false;
        state.navigation_completed_registered = false;
        state.navigation_starting_registered = false;
        state.cursor_changed_registered = false;
        return SAO_STATUS_OK;
    }
    if (state.view && state.process_failed_registered) {
        const HRESULT status = state.view->remove_ProcessFailed(state.process_failed_token);
        if (FAILED(status))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.process_failed_registered = false;
    }
    if (state.view && state.web_message_registered) {
        const HRESULT status = state.view->remove_WebMessageReceived(state.web_message_token);
        if (FAILED(status))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.web_message_registered = false;
    }
    if (state.view && state.navigation_completed_registered) {
        const HRESULT status =
            state.view->remove_NavigationCompleted(state.navigation_completed_token);
        if (FAILED(status))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.navigation_completed_registered = false;
    }
    if (state.view && state.navigation_starting_registered) {
        const HRESULT status =
            state.view->remove_NavigationStarting(state.navigation_starting_token);
        if (FAILED(status))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.navigation_starting_registered = false;
    }
    if (state.composition_controller && state.cursor_changed_registered) {
        const HRESULT status = state.composition_controller->remove_CursorChanged(
            state.cursor_changed_token);
        if (FAILED(status))
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        state.cursor_changed_registered = false;
    }
    return SAO_STATUS_OK;
}

} // namespace

struct CompositionHost {
    std::shared_ptr<HostState> state;
};

sao_status_t create(sao_ui_compositor_handle_t compositor,
                    sao_ai_editor_launcher_t launcher,
                    CompositionHost** out_host) noexcept {
    if (out_host == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;
    if (compositor == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    const sao_status_t owner_status = sao_ui_compositor_require_owner_thread(compositor);
    if (owner_status != SAO_STATUS_OK)
        return owner_status;
    try {
        auto result = std::unique_ptr<CompositionHost>(new (std::nothrow) CompositionHost{});
        if (!result)
            return SAO_STATUS_ERR_UNKNOWN;
        auto state = std::make_shared<HostState>();
        result->state = state;
        state->compositor = compositor;
        state->launcher = launcher;
        state->owner_thread = GetCurrentThreadId();
        state->parent_window = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        if (state->parent_window == nullptr)
            return SAO_STATUS_ERR_NOT_INITIALIZED;

        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com_status))
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        state->com_initialized = true;

        const std::filesystem::path module_dir = module_directory();
        state->asset_root = workbench_asset_root(module_dir);
        state->profile_root = user_data_folder();
        if (state->asset_root.empty() || state->profile_root.empty()) {
            CoUninitialize();
            state->com_initialized = false;
            return SAO_STATUS_ERR_NOT_FOUND;
        }
        const auto loader_path = module_dir / L"WebView2Loader.dll";
        state->loader = LoadLibraryW(loader_path.c_str());
        if (state->loader == nullptr) {
            CoUninitialize();
            state->com_initialized = false;
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        }
        state->create_environment = reinterpret_cast<CreateEnvironmentFn>(
            GetProcAddress(state->loader, "CreateCoreWebView2EnvironmentWithOptions"));
        if (state->create_environment == nullptr) {
            FreeLibrary(state->loader);
            state->loader = nullptr;
            CoUninitialize();
            state->com_initialized = false;
            return SAO_STATUS_ERR_NOT_IMPLEMENTED;
        }

        const auto overlay = sao_ui_compositor_host(compositor);
        SaoOverlayHostClientRect client{};
        sao_status_t status = sao_ui_overlay_host_get_client_rect(overlay, &client);
        if (status != SAO_STATUS_OK)
            return status;
        state->host_x = client.x;
        state->host_y = client.y;
        state->width = std::max(1, client.width);
        state->height = std::max(1, client.height);
        (void)sao_ui_compositor_host_dpi(compositor, &state->dpi, nullptr);
        SaoUiCompositionSlotConfig slot_config{};
        slot_config.struct_size = sizeof(slot_config);
        slot_config.name_utf8 = "sao.ai_editor.workbench";
        slot_config.width = state->width;
        slot_config.height = state->height;
        slot_config.z_order = kSlotZOrder;
        slot_config.band = SAO_UI_COMPOSITION_BAND_ABOVE_NATIVE;
        slot_config.opacity = 1.0F;
        slot_config.visible = false;
        slot_config.input_enabled = false;
        slot_config.focusable = true;
        status = sao_ui_composition_slot_create(compositor, &slot_config, &state->slot);
        if (status != SAO_STATUS_OK)
            return status;
        status = sao_ui_composition_slot_set_mouse_handler(state->slot, &mouse_callback,
                                                           state.get());
        if (status != SAO_STATUS_OK) {
            fail_state(*state, status);
            *out_host = result.release();
            return SAO_STATUS_OK;
        }
        status = start_environment(state);
        if (status != SAO_STATUS_OK) {
            fail_state(*state, status);
            *out_host = result.release();
            return SAO_STATUS_OK;
        }
        *out_host = result.release();
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

sao_status_t show(CompositionHost* host) noexcept {
    if (host == nullptr || !host->state)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto& state = *host->state;
    if (state.owner_thread != GetCurrentThreadId())
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (state.phase == HostState::Phase::failed)
        return state.failure_status;
    if (state.phase == HostState::Phase::closing)
        return SAO_STATUS_ERR_CANCELLED;
    state.requested_visible = true;
    const sao_status_t status = apply_requested_visibility(state);
    if (transient_composition_status(status))
        return SAO_STATUS_OK;
    if (status != SAO_STATUS_OK)
        fail_state(state, status);
    return status;
}

sao_status_t hide(CompositionHost* host) noexcept {
    if (host == nullptr || !host->state)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto& state = *host->state;
    if (state.owner_thread != GetCurrentThreadId())
        return SAO_STATUS_ERR_ACCESS_DENIED;
    state.requested_visible = false;
    cancel_mouse_state(state);
    const sao_status_t status = apply_requested_visibility(state);
    if (transient_composition_status(status))
        return SAO_STATUS_OK;
    if (status != SAO_STATUS_OK)
        fail_state(state, status);
    return status;
}

sao_status_t tick(CompositionHost* host) noexcept {
    if (host == nullptr || !host->state)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto& state = *host->state;
    if (state.owner_thread != GetCurrentThreadId())
        return SAO_STATUS_ERR_ACCESS_DENIED;
    if (state.phase == HostState::Phase::failed)
        return state.failure_status;
    if (state.phase == HostState::Phase::closing)
        return SAO_STATUS_ERR_CANCELLED;
    if (state.input_failure_status != SAO_STATUS_OK) {
        const sao_status_t status = state.input_failure_status;
        cancel_mouse_state(state);
        fail_state(state, status);
        return status;
    }
    if (state.navigation_started &&
        (!state.handshake_complete ||
         state.handshake_generation != state.navigation_generation) &&
        state.hello_deadline.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() >= state.hello_deadline) {
        fail_state(state, SAO_STATUS_ERR_TIMEOUT);
        return SAO_STATUS_ERR_TIMEOUT;
    }
    if (!state.controller)
        return SAO_STATUS_OK;
    sao_status_t status = update_bounds_and_dpi(state);
    if (transient_composition_status(status))
        return SAO_STATUS_OK;
    if (status != SAO_STATUS_OK) {
        fail_state(state, status);
        return status;
    }
    status = bind_current_target(state);
    if (transient_composition_status(status))
        return SAO_STATUS_OK;
    if (status != SAO_STATUS_OK)
        fail_state(state, status);
    return status;
}

sao_status_t try_destroy(CompositionHost* host) noexcept {
    if (host == nullptr)
        return SAO_STATUS_OK;
    if (!host->state)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    auto state = host->state;
    if (state->owner_thread != GetCurrentThreadId())
        return SAO_STATUS_ERR_ACCESS_DENIED;
    state->phase = HostState::Phase::closing;
    state->requested_visible = false;
    cancel_mouse_state(*state);
    (void)apply_requested_visibility(*state);
    if (state->pending_async != 0 || state->callback_depth != 0)
        return SAO_STATUS_ERR_CANCELLED;
    sao_status_t status = remove_handlers(*state);
    if (status != SAO_STATUS_OK)
        return status;
    if (state->composition_controller) {
        const HRESULT detach_status =
            state->composition_controller->put_RootVisualTarget(nullptr);
        if (FAILED(detach_status) && !state->process_failed)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
        if (SUCCEEDED(detach_status)) {
            status = sao_ui_composition_slot_commit(state->slot);
            if (status != SAO_STATUS_OK && !transient_composition_status(status) &&
                status != SAO_STATUS_ERR_HANDLE_INVALID)
                return status;
        }
        state->bound_target_generation = 0;
    }
    if (state->controller) {
        const HRESULT close_status = state->controller->Close();
        if (FAILED(close_status) && !state->process_failed)
            return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    state->view.Reset();
    state->controller4.Reset();
    state->controller.Reset();
    state->composition_controller.Reset();
    state->environment3.Reset();
    state->environment.Reset();
    state->environment_handler.Reset();
    if (state->slot != nullptr) {
        status = sao_ui_composition_slot_set_mouse_handler(state->slot, nullptr, nullptr);
        if (status != SAO_STATUS_OK)
            return status;
        status = sao_ui_composition_slot_try_destroy(state->slot);
        if (status != SAO_STATUS_OK)
            return status;
        state->slot = nullptr;
    }
    if (state->loader != nullptr) {
        FreeLibrary(state->loader);
        state->loader = nullptr;
    }
    if (state->com_initialized) {
        CoUninitialize();
        state->com_initialized = false;
    }
    host->state.reset();
    delete host;
    return SAO_STATUS_OK;
}

bool available(const CompositionHost* host) noexcept {
    return host != nullptr && host->state &&
           host->state->phase != HostState::Phase::failed &&
           host->state->phase != HostState::Phase::closing;
}

bool ready(const CompositionHost* host) noexcept {
    return host != nullptr && host->state && host->state->handshake_complete &&
           host->state->navigation_completed &&
           host->state->navigation_generation != 0 &&
           host->state->handshake_generation == host->state->navigation_generation &&
           host->state->phase == HostState::Phase::ready;
}

bool failed(const CompositionHost* host) noexcept {
    return host == nullptr || !host->state || host->state->phase == HostState::Phase::failed;
}

bool consume_close_request(CompositionHost* host) noexcept {
    if (host == nullptr || !host->state || !host->state->close_requested)
        return false;
    host->state->close_requested = false;
    return true;
}

} // namespace sao::ai_editor::workbench

#else

namespace sao::ai_editor::workbench {

struct CompositionHost {};

sao_status_t create(sao_ui_compositor_handle_t, sao_ai_editor_launcher_t,
                    CompositionHost** out_host) noexcept {
    if (out_host == nullptr)
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_host = nullptr;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
}

sao_status_t show(CompositionHost*) noexcept { return SAO_STATUS_ERR_NOT_IMPLEMENTED; }
sao_status_t hide(CompositionHost*) noexcept { return SAO_STATUS_ERR_NOT_IMPLEMENTED; }
sao_status_t tick(CompositionHost*) noexcept { return SAO_STATUS_ERR_NOT_IMPLEMENTED; }
sao_status_t try_destroy(CompositionHost* host) noexcept {
    delete host;
    return SAO_STATUS_OK;
}
bool available(const CompositionHost*) noexcept { return false; }
bool ready(const CompositionHost*) noexcept { return false; }
bool failed(const CompositionHost*) noexcept { return true; }
bool consume_close_request(CompositionHost*) noexcept { return false; }

} // namespace sao::ai_editor::workbench

#endif
