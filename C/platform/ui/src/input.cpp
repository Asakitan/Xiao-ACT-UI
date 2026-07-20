#include "sao/ui/input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include "input_win32_api.h"
#endif

struct InputHotkey {
    uint32_t id{0};
    uint32_t virtual_key{0};
    uint32_t modifiers{0};
    sao_ui_hotkey_callback_t callback{nullptr};
    void* user_data{nullptr};
    uint64_t order{0};
};

struct InputHookContext {
    std::mutex mu;
    std::vector<InputHotkey> hotkeys;
    uint64_t next_hotkey_order = 0;
    sao_ui_low_level_hook_t mouse_callback = nullptr;
    sao_ui_low_level_hook_t keyboard_callback = nullptr;
    void* ll_user_data = nullptr;
    bool accepting_callbacks = false;
    size_t in_flight_callbacks = 0;
    std::condition_variable callbacks_drained;
#if defined(_WIN32)
    HHOOK mouse_hook = nullptr;
    HHOOK keyboard_hook = nullptr;
    DWORD hook_thread_id = 0;
    bool install_in_progress = false;
    bool teardown_in_progress = false;
    std::condition_variable install_finished;
#endif
};

struct sao_ui_input_router_s {
    std::mutex mu;
    sao_ui_overlay_host_handle_t host = nullptr;
    std::vector<SaoOverlayHostInputRect> regions;
    std::vector<void*> shielded_windows;
    int32_t cursor_kind = SAO_UI_CURSOR_ARROW;
    std::unordered_map<std::string, int32_t> layer_cursors;
    std::shared_ptr<InputHookContext> hook_context;
};

namespace {

bool valid_cursor(int32_t cursor_kind) {
    return cursor_kind >= SAO_UI_CURSOR_ARROW && cursor_kind <= SAO_UI_CURSOR_HIDDEN;
}

#if defined(_WIN32)
using sao::ui::input_detail::Win32Api;

const Win32Api& system_win32_api() {
    static const Win32Api api{
        &::SetWindowsHookExW,
        &::UnhookWindowsHookEx,
        &::CallNextHookEx,
        &::GetAsyncKeyState,
        &::GetCurrentThreadId,
    };
    return api;
}

std::atomic<const Win32Api*>& win32_api_override() {
    static std::atomic<const Win32Api*> api{nullptr};
    return api;
}

const Win32Api& win32_api() {
    const auto* override_api = win32_api_override().load(std::memory_order_acquire);
    return override_api != nullptr ? *override_api : system_win32_api();
}

std::mutex& global_hook_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<InputHookContext>& active_hook_context() {
    static std::shared_ptr<InputHookContext> context;
    return context;
}

std::vector<std::shared_ptr<InputHookContext>>& retired_hook_contexts() {
    static std::vector<std::shared_ptr<InputHookContext>> contexts;
    return contexts;
}

void retire_hook_context_locked(
    const std::shared_ptr<InputHookContext>& context) {
    auto& retired = retired_hook_contexts();
    if (std::find(retired.begin(), retired.end(), context) == retired.end()) {
        retired.push_back(context);
    }
}

void unretire_hook_context_locked(
    const std::shared_ptr<InputHookContext>& context) {
    std::erase(retired_hook_contexts(), context);
}

struct CallbackThreadState {
    std::array<InputHookContext*, 16> stack{};
    size_t depth = 0;
};

CallbackThreadState& callback_thread_state() {
    thread_local CallbackThreadState state;
    return state;
}

size_t current_callback_depth(const InputHookContext* context) {
    const auto& state = callback_thread_state();
    return static_cast<size_t>(std::count(
        state.stack.begin(), state.stack.begin() + state.depth, context));
}

class CallbackLease final {
public:
    explicit CallbackLease(std::shared_ptr<InputHookContext> context) noexcept
        : context_(std::move(context)) {
        if (context_ == nullptr) return;
        auto& thread_state = callback_thread_state();
        if (thread_state.depth >= thread_state.stack.size()) return;
        std::lock_guard<std::mutex> lock(context_->mu);
        if (!context_->accepting_callbacks) return;
        thread_state.stack[thread_state.depth++] = context_.get();
        ++context_->in_flight_callbacks;
        acquired_ = true;
    }

    ~CallbackLease() {
        if (!acquired_) return;
        {
            std::lock_guard<std::mutex> lock(context_->mu);
            --context_->in_flight_callbacks;
            context_->callbacks_drained.notify_all();
        }
        auto& thread_state = callback_thread_state();
        --thread_state.depth;
        thread_state.stack[thread_state.depth] = nullptr;
    }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

    explicit operator bool() const noexcept { return acquired_; }

private:
    std::shared_ptr<InputHookContext> context_;
    bool acquired_ = false;
};

uint32_t current_modifiers() {
    uint32_t result = 0;
    if ((win32_api().get_async_key_state(VK_CONTROL) & 0x8000) != 0) {
        result |= SAO_UI_MOD_CTRL;
    }
    if ((win32_api().get_async_key_state(VK_MENU) & 0x8000) != 0) {
        result |= SAO_UI_MOD_ALT;
    }
    if ((win32_api().get_async_key_state(VK_SHIFT) & 0x8000) != 0) {
        result |= SAO_UI_MOD_SHIFT;
    }
    if ((win32_api().get_async_key_state(VK_LWIN) & 0x8000) != 0 ||
        (win32_api().get_async_key_state(VK_RWIN) & 0x8000) != 0) {
        result |= SAO_UI_MOD_WIN;
    }
    return result;
}

int modifier_count(uint32_t modifiers) {
    int result = 0;
    for (; modifiers != 0; modifiers &= modifiers - 1) ++result;
    return result;
}

void dispatch_registered_hotkey(const std::shared_ptr<InputHookContext>& context,
                                uint32_t virtual_key) {
    sao_ui_hotkey_callback_t callback = nullptr;
    void* user_data = nullptr;
    uint32_t hotkey_id = 0;
    const uint32_t observed = current_modifiers();
    {
        std::lock_guard<std::mutex> lock(context->mu);
        if (!context->accepting_callbacks) return;
        int best_specificity = -1;
        uint64_t best_order = UINT64_MAX;
        for (const auto& hotkey : context->hotkeys) {
            if (hotkey.virtual_key != virtual_key ||
                (hotkey.modifiers & observed) != hotkey.modifiers) continue;
            const int specificity = modifier_count(hotkey.modifiers);
            if (specificity > best_specificity ||
                (specificity == best_specificity && hotkey.order < best_order)) {
                best_specificity = specificity;
                best_order = hotkey.order;
                callback = hotkey.callback;
                user_data = hotkey.user_data;
                hotkey_id = hotkey.id;
            }
        }
    }
    if (callback != nullptr) callback(hotkey_id, user_data);
}

LRESULT CALLBACK low_level_mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
    std::shared_ptr<InputHookContext> context;
    sao_ui_low_level_hook_t callback = nullptr;
    void* user_data = nullptr;
    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        context = active_hook_context();
    }
    CallbackLease lease(context);
    if (lease) {
        std::lock_guard<std::mutex> lock(context->mu);
        callback = context->mouse_callback;
        user_data = context->ll_user_data;
    }
    if (code >= HC_ACTION && callback != nullptr &&
        callback(static_cast<uint32_t>(code), static_cast<uint64_t>(wparam),
                 static_cast<uint64_t>(lparam), user_data)) return 1;
    return win32_api().call_next_hook_ex(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK low_level_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    std::shared_ptr<InputHookContext> context;
    sao_ui_low_level_hook_t callback = nullptr;
    void* user_data = nullptr;
    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        context = active_hook_context();
    }
    CallbackLease lease(context);
    if (lease) {
        std::lock_guard<std::mutex> lock(context->mu);
        callback = context->keyboard_callback;
        user_data = context->ll_user_data;
    }
    bool consumed = false;
    if (code >= HC_ACTION && lease) {
        if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) {
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
            if (event != nullptr) dispatch_registered_hotkey(context, event->vkCode);
        }
        {
            std::lock_guard<std::mutex> lock(context->mu);
            if (!context->accepting_callbacks) {
                callback = nullptr;
                user_data = nullptr;
            }
        }
        if (callback != nullptr) {
            consumed = callback(static_cast<uint32_t>(code),
                                static_cast<uint64_t>(wparam),
                                static_cast<uint64_t>(lparam), user_data);
        }
    }
    return consumed ? 1 : win32_api().call_next_hook_ex(nullptr, code, wparam, lparam);
}

LPCWSTR cursor_id(int32_t cursor_kind) {
    switch (cursor_kind) {
    case SAO_UI_CURSOR_HAND:
        return IDC_HAND;
    case SAO_UI_CURSOR_TEXT:
        return IDC_IBEAM;
    case SAO_UI_CURSOR_RESIZE_NS:
        return IDC_SIZENS;
    case SAO_UI_CURSOR_RESIZE_EW:
        return IDC_SIZEWE;
    case SAO_UI_CURSOR_RESIZE_NWSE:
        return IDC_SIZENWSE;
    case SAO_UI_CURSOR_RESIZE_NESW:
        return IDC_SIZENESW;
    case SAO_UI_CURSOR_CROSSHAIR:
        return IDC_CROSS;
    case SAO_UI_CURSOR_WAIT:
        return IDC_WAIT;
    case SAO_UI_CURSOR_ARROW:
    default:
        return IDC_ARROW;
    }
}

void apply_cursor(int32_t cursor_kind) {
    if (cursor_kind == SAO_UI_CURSOR_HIDDEN) {
        while (::ShowCursor(FALSE) >= 0) {
        }
        return;
    }
    while (::ShowCursor(TRUE) < 0) {
    }
    ::SetCursor(::LoadCursorW(nullptr, cursor_id(cursor_kind)));
}

sao_status_t uninstall_hooks(const std::shared_ptr<InputHookContext>& context) {
    if (context == nullptr) return SAO_STATUS_OK;

    HHOOK mouse_hook = nullptr;
    HHOOK keyboard_hook = nullptr;
    {
        std::unique_lock<std::mutex> global_lock(global_hook_mutex());
        context->install_finished.wait(global_lock, [&context] {
            return !context->install_in_progress;
        });
        if (context->teardown_in_progress) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        context->teardown_in_progress = true;
        if (active_hook_context() == context) active_hook_context().reset();
        mouse_hook = context->mouse_hook;
        keyboard_hook = context->keyboard_hook;
    }
    {
        std::unique_lock<std::mutex> lock(context->mu);
        context->accepting_callbacks = false;
        const size_t own_callbacks = current_callback_depth(context.get());
        context->callbacks_drained.wait(lock, [&context, own_callbacks] {
            return context->in_flight_callbacks <= own_callbacks;
        });
    }

    const bool mouse_ok = mouse_hook == nullptr ||
        win32_api().unhook_windows_hook_ex(mouse_hook) != FALSE;
    const bool keyboard_ok = keyboard_hook == nullptr ||
        win32_api().unhook_windows_hook_ex(keyboard_hook) != FALSE;

    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        if (mouse_ok && context->mouse_hook == mouse_hook) {
            context->mouse_hook = nullptr;
        }
        if (keyboard_ok && context->keyboard_hook == keyboard_hook) {
            context->keyboard_hook = nullptr;
        }
        if (context->mouse_hook == nullptr && context->keyboard_hook == nullptr) {
            context->hook_thread_id = 0;
        }
        context->teardown_in_progress = false;
        if (mouse_ok && keyboard_ok) {
            unretire_hook_context_locked(context);
        } else {
            retire_hook_context_locked(context);
        }
    }
    return mouse_ok && keyboard_ok ? SAO_STATUS_OK
                                   : SAO_STATUS_ERR_OS_CALL_FAILED;
}

bool retry_retired_hooks() {
    std::vector<std::shared_ptr<InputHookContext>> snapshot;
    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        snapshot = retired_hook_contexts();
    }
    bool succeeded = true;
    for (const auto& context : snapshot) {
        if (uninstall_hooks(context) != SAO_STATUS_OK) succeeded = false;
    }
    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        auto& retired = retired_hook_contexts();
        std::erase_if(retired, [](const auto& context) {
            return context->mouse_hook == nullptr &&
                   context->keyboard_hook == nullptr &&
                   !context->teardown_in_progress;
        });
        succeeded = succeeded && retired.empty();
    }
    return succeeded;
}
#endif

}  // namespace

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_create(
    sao_ui_overlay_host_handle_t host, sao_ui_input_router_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (host == nullptr || sao_ui_overlay_host_hwnd(host) == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto* router = new (std::nothrow) sao_ui_input_router_s();
    if (router == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    try {
        router->hook_context = std::make_shared<InputHookContext>();
    } catch (...) {
        delete router;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    router->host = host;
    *out_handle = router;
    return SAO_STATUS_OK;
}

extern "C" void SAO_UI_CALL sao_ui_input_router_destroy(sao_ui_input_router_handle_t handle) {
#if defined(_WIN32)
    if (handle != nullptr) {
        const auto context = handle->hook_context;
        if (uninstall_hooks(context) != SAO_STATUS_OK) {
            std::lock_guard<std::mutex> global_lock(global_hook_mutex());
            auto& retired = retired_hook_contexts();
            if (std::find(retired.begin(), retired.end(), context) == retired.end()) {
                retired.push_back(context);
            }
        }
    }
#endif
    delete handle;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_set_regions(
    sao_ui_input_router_handle_t handle, const SaoOverlayHostInputRect* rects, size_t rect_count) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (rect_count != 0 && rects == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::vector<SaoOverlayHostInputRect> next;
    next.reserve(rect_count);
    for (size_t index = 0; index < rect_count; ++index) {
        if (rects[index].width <= 0 || rects[index].height <= 0) {
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        next.push_back(rects[index]);
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    handle->regions = std::move(next);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_rebuild_region(
    sao_ui_input_router_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::vector<SaoOverlayHostInputRect> snapshot;
    sao_ui_overlay_host_handle_t host = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        snapshot = handle->regions;
        host = handle->host;
    }
    return sao_ui_overlay_host_set_input_region(host, snapshot.data(), snapshot.size());
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_shield_arm_once(
    sao_ui_input_router_handle_t handle, void* proxy_hwnd) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (proxy_hwnd == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#if defined(_WIN32)
    if (!::IsWindow(reinterpret_cast<HWND>(proxy_hwnd))) return SAO_STATUS_ERR_INVALID_ARGUMENT;
#endif
    std::lock_guard<std::mutex> lock(handle->mu);
    const auto found = std::find(handle->shielded_windows.begin(), handle->shielded_windows.end(), proxy_hwnd);
    if (found != handle->shielded_windows.end()) return SAO_STATUS_ERR_ALREADY_EXISTS;
    // The shared overlay WndProc handles WM_MOUSEACTIVATE.  Recording the
    // arm-once state avoids subclass chains and leaves proxy ownership intact.
    handle->shielded_windows.push_back(proxy_hwnd);
    return SAO_STATUS_OK;
}

extern "C" bool SAO_UI_CALL sao_ui_input_router_shield_armed(
    sao_ui_input_router_handle_t handle, void* proxy_hwnd) {
    if (handle == nullptr || proxy_hwnd == nullptr) return false;
    std::lock_guard<std::mutex> lock(handle->mu);
    return std::find(handle->shielded_windows.begin(), handle->shielded_windows.end(), proxy_hwnd) !=
           handle->shielded_windows.end();
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_list_hotkeys(
    sao_ui_input_router_handle_t handle, SaoHotkeyBinding* out_bindings,
    size_t capacity, size_t* out_count) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_count == nullptr || (out_bindings == nullptr && capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto context = handle->hook_context;
    std::lock_guard<std::mutex> lock(context->mu);
    *out_count = context->hotkeys.size();
    const size_t copied = std::min(capacity, context->hotkeys.size());
    for (size_t index = 0; index < copied; ++index) {
        out_bindings[index] = {context->hotkeys[index].id,
                               context->hotkeys[index].virtual_key,
                               context->hotkeys[index].modifiers, 0};
    }
    return capacity < context->hotkeys.size() ? SAO_STATUS_ERR_BUFFER_TOO_SMALL
                                              : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_register_global_hotkey(
    sao_ui_input_router_handle_t handle, uint32_t hotkey_id,
    uint32_t virtual_key, uint32_t modifier_mask,
    sao_ui_hotkey_callback_t callback, void* user_data) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (hotkey_id == 0 || virtual_key == 0 || callback == nullptr ||
        (modifier_mask & ~(SAO_UI_MOD_CTRL | SAO_UI_MOD_ALT |
                           SAO_UI_MOD_SHIFT | SAO_UI_MOD_WIN)) != 0) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const auto context = handle->hook_context;
    std::lock_guard<std::mutex> lock(context->mu);
    if (std::any_of(context->hotkeys.begin(), context->hotkeys.end(),
                    [hotkey_id](const auto& item) { return item.id == hotkey_id; })) {
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    context->hotkeys.push_back({hotkey_id, virtual_key, modifier_mask,
                                callback, user_data, context->next_hotkey_order++});
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_unregister_global_hotkey(
    sao_ui_input_router_handle_t handle, uint32_t hotkey_id) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto context = handle->hook_context;
    std::lock_guard<std::mutex> lock(context->mu);
    const auto found = std::find_if(context->hotkeys.begin(), context->hotkeys.end(),
        [hotkey_id](const auto& item) { return item.id == hotkey_id; });
    if (found == context->hotkeys.end()) return SAO_STATUS_ERR_NOT_FOUND;
    context->hotkeys.erase(found);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_set_cursor(
    sao_ui_input_router_handle_t handle, int32_t cursor_kind) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_cursor(cursor_kind)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        handle->cursor_kind = cursor_kind;
    }
#if defined(_WIN32)
    apply_cursor(cursor_kind);
#endif
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_set_layer_cursor(
    sao_ui_input_router_handle_t handle, const char* layer_name_utf8, int32_t cursor_kind) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (layer_name_utf8 == nullptr || layer_name_utf8[0] == '\0' || !valid_cursor(cursor_kind)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(handle->mu);
    if (cursor_kind == SAO_UI_CURSOR_ARROW) {
        handle->layer_cursors.erase(layer_name_utf8);
    } else {
        handle->layer_cursors[layer_name_utf8] = cursor_kind;
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_install_ll_hooks(
    sao_ui_input_router_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(_WIN32)
    // CAPABILITY GATE: requires SetWindowsHookExW (user32.dll,
    // Windows-only WH_MOUSE_LL / WH_KEYBOARD_LL).  See PLAN.md §1.5.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#else
    if (!retry_retired_hooks()) return SAO_STATUS_ERR_OS_CALL_FAILED;
    const auto context = handle->hook_context;
    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        if (context->install_in_progress || context->teardown_in_progress) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        if (context->mouse_hook != nullptr || context->keyboard_hook != nullptr) {
            return active_hook_context() == context ? SAO_STATUS_OK
                                                    : SAO_STATUS_ERR_OS_CALL_FAILED;
        }
        if (active_hook_context() != nullptr && active_hook_context() != context) {
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
        context->install_in_progress = true;
    }

    const HHOOK mouse_hook = win32_api().set_windows_hook_ex_w(
        WH_MOUSE_LL, low_level_mouse_proc, nullptr, 0);
    if (mouse_hook == nullptr) {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        context->install_in_progress = false;
        context->install_finished.notify_all();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    const HHOOK keyboard_hook = win32_api().set_windows_hook_ex_w(
        WH_KEYBOARD_LL, low_level_keyboard_proc, nullptr, 0);
    if (keyboard_hook == nullptr) {
        const bool rollback_ok =
            win32_api().unhook_windows_hook_ex(mouse_hook) != FALSE;
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        if (!rollback_ok) {
            context->mouse_hook = mouse_hook;
            retire_hook_context_locked(context);
        }
        context->install_in_progress = false;
        context->install_finished.notify_all();
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }

    {
        std::lock_guard<std::mutex> lock(context->mu);
        context->accepting_callbacks = true;
    }
    {
        std::lock_guard<std::mutex> global_lock(global_hook_mutex());
        context->mouse_hook = mouse_hook;
        context->keyboard_hook = keyboard_hook;
        context->hook_thread_id = win32_api().get_current_thread_id();
        context->install_in_progress = false;
        active_hook_context() = context;
        context->install_finished.notify_all();
    }
    return SAO_STATUS_OK;
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_uninstall_ll_hooks(
    sao_ui_input_router_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
#if !defined(_WIN32)
    // CAPABILITY GATE: requires UnhookWindowsHookEx (user32.dll,
    // Windows-only).  See PLAN.md §1.5.
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
#else
    return uninstall_hooks(handle->hook_context);
#endif
}

extern "C" sao_status_t SAO_UI_CALL sao_ui_input_router_set_ll_hook_callbacks(
    sao_ui_input_router_handle_t handle,
    sao_ui_low_level_hook_t mouse_callback,
    sao_ui_low_level_hook_t keyboard_callback,
    void* user_data) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto context = handle->hook_context;
    std::lock_guard<std::mutex> lock(context->mu);
    context->mouse_callback = mouse_callback;
    context->keyboard_callback = keyboard_callback;
    context->ll_user_data = user_data;
    return SAO_STATUS_OK;
}

#if defined(_WIN32) && defined(SAO_UI_INPUT_TESTING)
namespace sao::ui::input_detail {

void set_win32_api_for_testing(const Win32Api* api) noexcept {
    win32_api_override().store(api, std::memory_order_release);
}

void reset_win32_api_for_testing() noexcept {
    win32_api_override().store(nullptr, std::memory_order_release);
}

LRESULT invoke_mouse_proc_for_testing(int code, WPARAM wparam, LPARAM lparam) {
    return low_level_mouse_proc(code, wparam, lparam);
}

LRESULT invoke_keyboard_proc_for_testing(int code, WPARAM wparam, LPARAM lparam) {
    return low_level_keyboard_proc(code, wparam, lparam);
}

bool active_router_present_for_testing() noexcept {
    std::lock_guard<std::mutex> lock(global_hook_mutex());
    return active_hook_context() != nullptr;
}

std::uintptr_t mouse_hook_for_testing(sao_ui_input_router_handle_t handle) noexcept {
    if (handle == nullptr) return 0;
    std::lock_guard<std::mutex> lock(global_hook_mutex());
    return reinterpret_cast<std::uintptr_t>(handle->hook_context->mouse_hook);
}

std::uintptr_t keyboard_hook_for_testing(sao_ui_input_router_handle_t handle) noexcept {
    if (handle == nullptr) return 0;
    std::lock_guard<std::mutex> lock(global_hook_mutex());
    return reinterpret_cast<std::uintptr_t>(handle->hook_context->keyboard_hook);
}

bool retry_retired_hooks_for_testing() noexcept {
    return retry_retired_hooks();
}

}  // namespace sao::ui::input_detail
#endif
