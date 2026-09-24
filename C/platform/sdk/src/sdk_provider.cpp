#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "sao/core/thread.h"
#include "sao/engine/ui_spec.h"
#include "sao/ui/alerts.h"
#include "sao/ui/d2d_widgets.h"
#include "sao/ui/dialog.h"
#include "sao/ui/panel.h"

namespace sao_sdk_internal {

thread_local ContextState* g_memory_callback_owner = nullptr;
thread_local MemoryProviderSession* g_memory_callback_session = nullptr;

bool memory_callback_reentered(ContextState* state) noexcept {
    return state != nullptr && g_memory_callback_owner == state;
}

namespace {

sao_sdk_status_t map_tts_status(sao_status_t status) noexcept {
    switch (status) {
    case SAO_STATUS_OK:
        return SAO_SDK_OK;
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    case SAO_STATUS_ERR_NOT_INITIALIZED:
        return SAO_SDK_ERR_NOT_INITIALIZED;
    case SAO_STATUS_ERR_HANDLE_INVALID:
        return SAO_SDK_ERR_HANDLE_INVALID;
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return SAO_SDK_ERR_BUFFER_TOO_SMALL;
    case SAO_STATUS_ERR_NOT_IMPLEMENTED:
        return SAO_SDK_ERR_NOT_IMPLEMENTED;
    case SAO_STATUS_ERR_CANCELLED:
        return SAO_SDK_ERR_BUSY;
    case SAO_STATUS_ERR_ABI_MISMATCH:
        return SAO_SDK_ERR_ABI_MISMATCH;
    case SAO_STATUS_ERR_CAPABILITY_MISSING:
        return SAO_SDK_ERR_UNSUPPORTED;
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return SAO_SDK_ERR_ACCESS_DENIED;
    case SAO_STATUS_ERR_NOT_FOUND:
        return SAO_SDK_ERR_NOT_FOUND;
    case SAO_STATUS_ERR_ALREADY_EXISTS:
        return SAO_SDK_ERR_ALREADY_EXISTS;
    case SAO_STATUS_ERR_READ_FAULT:
        return SAO_SDK_ERR_READ_FAULT;
    case static_cast<sao_status_t>(-102):
        return SAO_SDK_ERR_BUSY;
    default:
        return SAO_SDK_ERR_INTERNAL;
    }
}

constexpr uint32_t kProviderMinimumSize =
    static_cast<uint32_t>(offsetof(SaoSdkProviderVTable, release) +
                          sizeof(static_cast<SaoSdkProviderVTable*>(nullptr)->release));
constexpr uint32_t kGpuHuntProviderMinimumSize =
    static_cast<uint32_t>(offsetof(SaoSdkProviderVTable, gpu_hunt_read) +
                          sizeof(static_cast<SaoSdkProviderVTable*>(nullptr)->gpu_hunt_read));
constexpr uint32_t kMemoryProviderMinimumSize = SAO_SDK_MEMORY_PROVIDER_REQUIRED_SIZE;
constexpr size_t kMaximumTtsUtf8Bytes = 64u * 1024u;

bool bounded_utf8_length(const char* value, size_t* out_length) noexcept {
    if (out_length == nullptr)
        return false;
    *out_length = 0u;
    if (value == nullptr)
        return false;
    for (size_t length = 0u; length < kMaximumTtsUtf8Bytes; ++length) {
        if (value[length] == '\0') {
            *out_length = length;
            return true;
        }
    }
    return false;
}

class MemoryCallbackScope {
  public:
    MemoryCallbackScope(ContextState* owner, MemoryProviderSession* session) noexcept
        : previous_owner_(g_memory_callback_owner), previous_session_(g_memory_callback_session) {
        g_memory_callback_owner = owner;
        g_memory_callback_session = session;
    }

    ~MemoryCallbackScope() {
        g_memory_callback_owner = previous_owner_;
        g_memory_callback_session = previous_session_;
    }

    MemoryCallbackScope(const MemoryCallbackScope&) = delete;
    MemoryCallbackScope& operator=(const MemoryCallbackScope&) = delete;

  private:
    ContextState* previous_owner_ = nullptr;
    MemoryProviderSession* previous_session_ = nullptr;
};

class MemoryCallLease {
  public:
    explicit MemoryCallLease(ContextState* state) {
        if (state == nullptr)
            return;
        if (state->destroying.load(std::memory_order_acquire)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        if (memory_callback_reentered(state)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mu);
            session_ = state->memory_provider;
        }
        if (session_ == nullptr) {
            status_ = SAO_SDK_ERR_UNSUPPORTED;
            return;
        }
        std::lock_guard<std::mutex> lock(session_->mutex);
        if (!session_->accepting) {
            status_ = session_->cleanup_status == SAO_SDK_OK ? SAO_SDK_ERR_BUSY
                                                             : session_->cleanup_status;
            session_.reset();
            return;
        }
        ++session_->active_calls;
        active_ = true;
        status_ = SAO_SDK_OK;
    }

    ~MemoryCallLease() {
        if (!active_)
            return;
        std::lock_guard<std::mutex> lock(session_->mutex);
        --session_->active_calls;
        if (session_->active_calls == 0)
            session_->idle.notify_all();
    }

    MemoryCallLease(const MemoryCallLease&) = delete;
    MemoryCallLease& operator=(const MemoryCallLease&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }
    sao_sdk_status_t status() const noexcept {
        return status_;
    }
    MemoryProviderSession* operator->() const noexcept {
        return session_.get();
    }

  private:
    std::shared_ptr<MemoryProviderSession> session_;
    bool active_ = false;
    sao_sdk_status_t status_ = SAO_SDK_ERR_HANDLE_INVALID;
};

thread_local ContextState* g_provider_callback_owner = nullptr;

class ProviderCallbackScope {
  public:
    explicit ProviderCallbackScope(ContextState* owner) noexcept
        : previous_owner_(g_provider_callback_owner) {
        g_provider_callback_owner = owner;
    }

    ~ProviderCallbackScope() {
        g_provider_callback_owner = previous_owner_;
    }

    ProviderCallbackScope(const ProviderCallbackScope&) = delete;
    ProviderCallbackScope& operator=(const ProviderCallbackScope&) = delete;

  private:
    ContextState* previous_owner_ = nullptr;
};

class ProviderCallLease {
  public:
    explicit ProviderCallLease(ContextState* state) : state_(state) {
        if (state_ == nullptr)
            return;
        if (state_->destroying.load(std::memory_order_acquire)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            if (!state_->provider_bound) {
                status_ = SAO_SDK_ERR_UNSUPPORTED;
                return;
            }
            provider_ = state_->provider;
        }
        std::lock_guard<std::mutex> lock(state_->provider_mutex);
        if (!state_->provider_accepting) {
            status_ = state_->provider_cleanup_status == SAO_SDK_OK
                          ? SAO_SDK_ERR_BUSY
                          : state_->provider_cleanup_status;
            return;
        }
        ++state_->provider_active_calls;
        active_ = true;
        status_ = SAO_SDK_OK;
    }

    ~ProviderCallLease() {
        if (!active_)
            return;
        std::lock_guard<std::mutex> lock(state_->provider_mutex);
        --state_->provider_active_calls;
        if (state_->provider_active_calls == 0)
            state_->provider_idle.notify_all();
    }

    ProviderCallLease(const ProviderCallLease&) = delete;
    ProviderCallLease& operator=(const ProviderCallLease&) = delete;

    explicit operator bool() const noexcept {
        return active_;
    }
    sao_sdk_status_t status() const noexcept {
        return status_;
    }
    const SaoSdkProviderVTable& provider() const noexcept {
        return provider_;
    }

  private:
    ContextState* state_ = nullptr;
    SaoSdkProviderVTable provider_{};
    bool active_ = false;
    sao_sdk_status_t status_ = SAO_SDK_ERR_HANDLE_INVALID;
};

template <typename Callback>
sao_sdk_status_t invoke_provider_callback(ContextState* state, Callback&& callback) noexcept {
    ProviderCallbackScope callback_scope(state);
    return invoke_callback_barrier(std::forward<Callback>(callback));
}

bool memory_session_attached(MemoryProviderSession* session) {
    std::lock_guard<std::mutex> lock(session->mutex);
    return session->attached;
}

sao_sdk_status_t shutdown_memory_session(const std::shared_ptr<MemoryProviderSession>& session) {
    if (session == nullptr)
        return SAO_SDK_OK;
    if (g_memory_callback_session == session.get() || memory_callback_reentered(session->owner))
        return SAO_SDK_ERR_BUSY;

    {
        std::unique_lock<std::mutex> lock(session->mutex);
        session->accepting = false;
        session->idle.wait(lock, [&session] { return session->active_calls == 0; });
    }

    std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        attached = session->attached;
    }
    if (attached) {
        const auto status = invoke_callback_barrier([&] {
            MemoryCallbackScope callback_scope(session->owner, session.get());
            return normalize_provider_status(
                session->provider.detach(session->provider.user_data, session->session));
        });
        if (status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cleanup_status = status;
            return status;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->attached = false;
    }

    if (session->session != nullptr) {
        const auto status = invoke_callback_barrier([&] {
            MemoryCallbackScope callback_scope(session->owner, session.get());
            return normalize_provider_status(
                session->provider.close_session(session->provider.user_data, session->session));
        });
        if (status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cleanup_status = status;
            return status;
        }
        session->session = nullptr;
    }

    if (session->retained) {
        const auto status = invoke_callback_barrier([&] {
            MemoryCallbackScope callback_scope(session->owner, session.get());
            session->provider.release(session->provider.user_data);
            return SAO_SDK_OK;
        });
        if (status == SAO_SDK_OK) {
            session->retained = false;
        } else {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cleanup_status = status;
            return status;
        }
    }

    try {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->cleanup_status = SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
    return SAO_SDK_OK;
}

struct TimerCallbackBridge {
    ContextState* owner = nullptr;
    sao_sdk_timer_token_t sdk_token = 0;
    sao_sdk_timer_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct HotkeyCallbackBridge {
    ContextState* owner = nullptr;
    sao_sdk_hotkey_id_t sdk_token = 0;
    sao_sdk_hotkey_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct DialogCallbackBridge {
    ContextState* owner = nullptr;
    sao_sdk_dialog_token_t sdk_token = 0;
    sao_sdk_dialog_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct RenderCallbackBridge {
    ContextState* owner = nullptr;
    sao_sdk_render_hook_callback_t callback = nullptr;
    void* user_data = nullptr;
};

void destroy_timer_bridge(void* value) {
    delete static_cast<TimerCallbackBridge*>(value);
}

void destroy_hotkey_provider_bridge(void* value) {
    delete static_cast<HotkeyCallbackBridge*>(value);
}

void destroy_dialog_bridge(void* value) {
    delete static_cast<DialogCallbackBridge*>(value);
}

void destroy_render_bridge(void* value) {
    delete static_cast<RenderCallbackBridge*>(value);
}

sao_sdk_status_t SAO_SDK_CALL render_callback_bridge(int32_t hook_point,
                                                     const SaoSdkRenderHookPayload* payload,
                                                     void* user_data) {
    auto* bridge = static_cast<RenderCallbackBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    PluginCallbackLease callback_lease(bridge->owner);
    if (!callback_lease)
        return SAO_SDK_ERR_BUSY;
    return invoke_callback_barrier([bridge, hook_point, payload] {
        return bridge->callback(hook_point, payload, bridge->user_data);
    });
}

void SAO_SDK_CALL timer_callback_bridge(sao_sdk_timer_token_t, void* user_data) {
    auto* bridge = static_cast<TimerCallbackBridge*>(user_data);
    if (bridge != nullptr && bridge->callback != nullptr) {
        PluginCallbackLease callback_lease(bridge->owner);
        if (callback_lease) {
            (void)invoke_void_callback_barrier(
                [bridge] { bridge->callback(bridge->sdk_token, bridge->user_data); });
        }
    }
}

void SAO_SDK_CALL hotkey_callback_bridge(sao_sdk_hotkey_id_t, void* user_data) {
    auto* bridge = static_cast<HotkeyCallbackBridge*>(user_data);
    if (bridge != nullptr && bridge->callback != nullptr) {
        PluginCallbackLease callback_lease(bridge->owner);
        if (callback_lease) {
            (void)invoke_void_callback_barrier(
                [bridge] { bridge->callback(bridge->sdk_token, bridge->user_data); });
        }
    }
}

void SAO_SDK_CALL dialog_callback_bridge(sao_sdk_dialog_token_t, int32_t pressed_button,
                                         const char* input_text_utf8, size_t input_text_len,
                                         void* user_data) {
    auto* bridge = static_cast<DialogCallbackBridge*>(user_data);
    if (bridge != nullptr && bridge->callback != nullptr) {
        PluginCallbackLease callback_lease(bridge->owner);
        if (callback_lease) {
            (void)invoke_void_callback_barrier(
                [bridge, pressed_button, input_text_utf8, input_text_len] {
                    bridge->callback(bridge->sdk_token, pressed_button, input_text_utf8,
                                     input_text_len, bridge->user_data);
                });
        }
    }
}

uint64_t allocate_capability_token(ContextState* state) {
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        return state->next_capability_token++;
    } catch (...) {
        return 0;
    }
}

sao_sdk_status_t add_registration(ContextState* state, CapabilityRegistration registration) {
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        state->capability_registrations.push_back(registration);
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

bool take_registration(ContextState* state, CapabilityKind kind, uint64_t sdk_token,
                       CapabilityRegistration* out_registration,
                       SaoSdkProviderVTable* out_provider) {
    std::lock_guard<std::mutex> lock(state->mu);
    const auto found =
        std::find_if(state->capability_registrations.begin(), state->capability_registrations.end(),
                     [kind, sdk_token](const CapabilityRegistration& registration) {
                         return registration.kind == kind && registration.sdk_token == sdk_token;
                     });
    if (found == state->capability_registrations.end())
        return false;
    if (found->unregistering)
        return false;
    found->unregistering = true;
    *out_registration = *found;
    *out_provider = state->provider;
    return true;
}

void restore_registration(ContextState* state, const CapabilityRegistration& registration) {
    std::lock_guard<std::mutex> lock(state->mu);
    const auto found = std::find_if(
        state->capability_registrations.begin(), state->capability_registrations.end(),
        [&registration](const CapabilityRegistration& item) {
            return item.kind == registration.kind && item.sdk_token == registration.sdk_token;
        });
    if (found != state->capability_registrations.end())
        found->unregistering = false;
}

sao_sdk_status_t unregister_provider_token(ContextState* state,
                                           const SaoSdkProviderVTable& provider,
                                           const CapabilityRegistration& registration) {
    sao_sdk_status_t status = SAO_SDK_ERR_UNSUPPORTED;
    switch (registration.kind) {
    case CapabilityKind::render_hook:
        if (provider.unregister_render_hook != nullptr) {
            status = invoke_provider_callback(state, [&] {
                return provider.unregister_render_hook(provider.user_data,
                                                       registration.provider_token);
            });
        }
        break;
    case CapabilityKind::timer:
        if (provider.unregister_timer != nullptr) {
            status = invoke_provider_callback(state, [&] {
                return provider.unregister_timer(provider.user_data, registration.provider_token);
            });
        }
        break;
    case CapabilityKind::hotkey:
        if (provider.unregister_hotkey != nullptr) {
            status = invoke_provider_callback(state, [&] {
                return provider.unregister_hotkey(provider.user_data, registration.provider_token);
            });
        }
        break;
    case CapabilityKind::dialog:
        if (provider.dismiss_dialog != nullptr) {
            status = invoke_provider_callback(state, [&] {
                return provider.dismiss_dialog(provider.user_data, registration.provider_token);
            });
        }
        break;
    case CapabilityKind::notify:
        if (provider.dismiss_notify != nullptr) {
            status = invoke_provider_callback(state, [&] {
                return provider.dismiss_notify(provider.user_data, registration.provider_token);
            });
        }
        break;
    case CapabilityKind::overlay:
        if (provider.clear_overlay != nullptr) {
            status = invoke_provider_callback(state, [&] {
                return provider.clear_overlay(provider.user_data, registration.provider_token);
            });
        }
        break;
    }
    return status;
}

void finish_registration(ContextState* state, const CapabilityRegistration& registration) {
    {
        std::unique_lock<std::mutex> lock(state->callback_gate->mutex);
        state->callback_gate->idle.wait(lock,
                                        [state] { return state->callback_gate->active == 0; });
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase_if(
            state->capability_registrations, [&registration](const CapabilityRegistration& item) {
                return item.kind == registration.kind && item.sdk_token == registration.sdk_token;
            });
        switch (registration.kind) {
        case CapabilityKind::render_hook:
            std::erase_if(state->render_hooks, [&registration](const RenderHookEntry& entry) {
                return entry.token == registration.sdk_token;
            });
            break;
        case CapabilityKind::hotkey:
            std::erase_if(state->hotkeys, [&registration](const HotkeyEntry& entry) {
                return entry.sdk_id == registration.sdk_token;
            });
            break;
        case CapabilityKind::notify:
            std::erase(state->banner_ids, registration.sdk_token);
            break;
        case CapabilityKind::overlay:
            std::erase_if(state->overlays, [&registration](const auto& item) {
                return reinterpret_cast<uint64_t>(item.second) == registration.sdk_token;
            });
            break;
        case CapabilityKind::timer:
        case CapabilityKind::dialog:
            break;
        }
    }
    if (registration.destroy_bridge != nullptr)
        registration.destroy_bridge(registration.bridge);
}

sao_sdk_status_t normalize_unregister_status(sao_sdk_status_t status);

sao_sdk_status_t rollback_added_registration(ContextState* state,
                                             const SaoSdkProviderVTable& provider,
                                             const CapabilityRegistration& registration,
                                             sao_sdk_status_t insertion_status) {
    const auto unregister_status =
        normalize_unregister_status(unregister_provider_token(state, provider, registration));
    if (unregister_status != SAO_SDK_OK)
        return unregister_status;
    finish_registration(state, registration);
    return insertion_status;
}

sao_sdk_status_t normalize_unregister_status(sao_sdk_status_t status) {
    if (status == SAO_SDK_OK || status == SAO_SDK_ERR_NOT_FOUND ||
        status == SAO_STATUS_ERR_SUBSCRIPTION_GONE) {
        return SAO_SDK_OK;
    }
    return status;
}

// Platform provider --------------------------------------------------

struct PlatformTimerEntry {
    sao_core_timer_handle_t timer = nullptr;
    sao_sdk_timer_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformRenderBridge {
    sao_sdk_render_hook_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformHotkeyEntry {
    sao_ui_hotkey_binding_t binding = 0;
    sao_sdk_hotkey_callback_t callback = nullptr;
    void* user_data = nullptr;
    CallbackActivity callback_activity;
};

struct PlatformDialogEntry {
    sao_ui_dialog_handle_t dialog = nullptr;
    sao_sdk_dialog_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformOverlayPaintNode {
    std::string panel_id;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct PlatformOverlayEntry {
    std::string plugin_id;
    std::string surface_id;
    std::string engine_spec;
    std::string panel_spec;
    std::vector<PlatformOverlayPaintNode> paint_nodes;
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_panel_handle_t panel = nullptr;
    sao_ui_layer_handle_t layer = nullptr;
    CallbackActivity callback_activity;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t z = 0;
    float drag_x = 0;
    float drag_y = 0;
    bool draggable = false;
    bool rect_hit = false;
    bool dragging = false;
    bool registry_owned = false;
};

struct PlatformGpuHuntProvider {
    SaoSdkProviderVTable provider{};
    bool retained = false;

    ~PlatformGpuHuntProvider() {
        if (!retained || provider.release == nullptr)
            return;
        (void)invoke_void_callback_barrier([this] { provider.release(provider.user_data); });
    }
};

struct PlatformGpuHuntSession {
    std::shared_ptr<PlatformGpuHuntProvider> owner;
    void* provider_session = nullptr;
    bool retained = false;
    ~PlatformGpuHuntSession() {
        if (owner == nullptr)
            return;
        void* session = std::exchange(provider_session, nullptr);
        if (session != nullptr) {
            (void)invoke_callback_barrier([this, session] {
                return owner->provider.gpu_hunt_close_session(owner->provider.user_data, session);
            });
        }
        if (std::exchange(retained, false)) {
            (void)invoke_void_callback_barrier(
                [this] { owner->provider.release(owner->provider.user_data); });
        }
    }
};

struct PlatformProviderState {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<PlatformTimerEntry>> timers;
    std::unordered_map<uint64_t, std::shared_ptr<PlatformHotkeyEntry>> hotkeys;
    std::unordered_map<uint64_t, std::shared_ptr<PlatformDialogEntry>> dialogs;
    std::unordered_map<uint64_t, std::shared_ptr<PlatformOverlayEntry>> overlays;
    bool overlay_operation_active = false;
    std::shared_ptr<PlatformGpuHuntProvider> gpu_hunt_provider;
    std::atomic<uint64_t> next_token{1};
};

std::atomic_bool g_fail_next_platform_timer_insertion{false};
std::atomic_bool g_fail_next_platform_hotkey_insertion{false};
std::atomic_bool g_fail_next_platform_dialog_insertion{false};
std::atomic_bool g_fail_next_platform_overlay_insertion{false};
std::atomic_bool g_fail_next_render_state_insertion{false};
std::atomic_bool g_fail_next_hotkey_state_insertion{false};
std::atomic_bool g_fail_next_notify_state_insertion{false};
std::atomic_bool g_fail_next_overlay_state_insertion{false};

PlatformProviderState& platform_provider_state() {
    static PlatformProviderState state;
    return state;
}

void SAO_SDK_CALL platform_provider_retain(void*) {}

void SAO_SDK_CALL platform_provider_release(void*) {}

std::shared_ptr<PlatformGpuHuntProvider> platform_gpu_hunt_provider_snapshot() {
    auto& state = platform_provider_state();
    std::lock_guard lock(state.mutex);
    return state.gpu_hunt_provider;
}

sao_sdk_status_t configure_platform_gpu_hunt_provider(const SaoSdkProviderVTable* provider) {
    std::shared_ptr<PlatformGpuHuntProvider> replacement;
    if (provider != nullptr) {
        if ((provider->abi_version >> 16) != SAO_SDK_PROVIDER_ABI_VERSION_MAJOR ||
            provider->struct_size < kGpuHuntProviderMinimumSize) {
            return SAO_SDK_ERR_ABI_MISMATCH;
        }

        SaoSdkProviderVTable copy{};
        std::memcpy(&copy, provider, std::min<size_t>(provider->struct_size, sizeof(copy)));
        if (copy.retain == nullptr || copy.release == nullptr ||
            copy.gpu_hunt_open_session == nullptr || copy.gpu_hunt_close_session == nullptr ||
            copy.gpu_hunt_attach == nullptr || copy.gpu_hunt_detach == nullptr ||
            copy.gpu_hunt_enum_regions == nullptr || copy.gpu_hunt_read == nullptr) {
            return SAO_SDK_ERR_UNSUPPORTED;
        }

        try {
            replacement = std::make_shared<PlatformGpuHuntProvider>();
            replacement->provider = copy;
        } catch (...) {
            return SAO_SDK_ERR_NOT_INITIALIZED;
        }
        const auto retain_status =
            invoke_void_callback_barrier([&copy] { copy.retain(copy.user_data); });
        if (retain_status != SAO_SDK_OK)
            return retain_status;
        replacement->retained = true;
    }

    std::shared_ptr<PlatformGpuHuntProvider> previous;
    {
        auto& state = platform_provider_state();
        std::lock_guard lock(state.mutex);
        previous = std::exchange(state.gpu_hunt_provider, std::move(replacement));
    }
    previous.reset();
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_gpu_hunt_open_session(void*, const char* plugin_id_utf8,
                                                             void** out_session) {
    if (out_session != nullptr)
        *out_session = nullptr;
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' || out_session == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    try {
        auto owner = platform_gpu_hunt_provider_snapshot();
        if (owner == nullptr)
            return SAO_SDK_ERR_UNSUPPORTED;

        auto session = std::make_unique<PlatformGpuHuntSession>();
        session->owner = std::move(owner);
        auto& provider = session->owner->provider;
        const auto retain_status =
            invoke_void_callback_barrier([&provider] { provider.retain(provider.user_data); });
        if (retain_status != SAO_SDK_OK)
            return retain_status;
        session->retained = true;
        const auto status = invoke_callback_barrier([&] {
            return provider.gpu_hunt_open_session(provider.user_data, plugin_id_utf8,
                                                  &session->provider_session);
        });
        if (status != SAO_SDK_OK || session->provider_session == nullptr) {
            void* provider_session = std::exchange(session->provider_session, nullptr);
            if (provider_session != nullptr) {
                (void)invoke_callback_barrier([&] {
                    return provider.gpu_hunt_close_session(provider.user_data, provider_session);
                });
            }
            return status == SAO_SDK_OK ? SAO_SDK_ERR_NOT_INITIALIZED : status;
        }

        *out_session = session.release();
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_gpu_hunt_close_session(void*, void* session_value) {
    if (session_value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* session = static_cast<PlatformGpuHuntSession*>(session_value);
    auto& provider = session->owner->provider;
    if (session->provider_session != nullptr) {
        const auto status = invoke_callback_barrier([&] {
            return provider.gpu_hunt_close_session(provider.user_data, session->provider_session);
        });
        if (status != SAO_SDK_OK)
            return status;
        session->provider_session = nullptr;
    }
    if (session->retained) {
        const auto status = invoke_callback_barrier([&] {
            provider.release(provider.user_data);
            return SAO_SDK_OK;
        });
        if (status != SAO_SDK_OK)
            return status;
        session->retained = false;
    }
    delete session;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_gpu_hunt_attach(void*, void* session_value, uint32_t pid) {
    if (session_value == nullptr || pid == 0) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* session = static_cast<PlatformGpuHuntSession*>(session_value);
    try {
        return session->owner->provider.gpu_hunt_attach(session->owner->provider.user_data,
                                                        session->provider_session, pid);
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_gpu_hunt_detach(void*, void* session_value) {
    if (session_value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* session = static_cast<PlatformGpuHuntSession*>(session_value);
    try {
        return session->owner->provider.gpu_hunt_detach(session->owner->provider.user_data,
                                                        session->provider_session);
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_gpu_hunt_enum_regions(void*, void* session_value,
                                                             SaoSdkGpuHuntRegion* out_regions,
                                                             size_t capacity, size_t* out_count) {
    if (session_value == nullptr || out_count == nullptr ||
        (capacity != 0 && out_regions == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* session = static_cast<PlatformGpuHuntSession*>(session_value);
    try {
        return session->owner->provider.gpu_hunt_enum_regions(session->owner->provider.user_data,
                                                              session->provider_session,
                                                              out_regions, capacity, out_count);
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_gpu_hunt_read(void*, void* session_value, uint64_t address,
                                                     uint8_t* out_buffer, size_t buffer_size,
                                                     size_t* out_bytes_read) {
    if (out_bytes_read != nullptr)
        *out_bytes_read = 0;
    if (session_value == nullptr || out_bytes_read == nullptr ||
        (buffer_size != 0 && out_buffer == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* session = static_cast<PlatformGpuHuntSession*>(session_value);
    try {
        return session->owner->provider.gpu_hunt_read(session->owner->provider.user_data,
                                                      session->provider_session, address,
                                                      out_buffer, buffer_size, out_bytes_read);
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

std::vector<uint16_t> utf8_to_utf16(const char* value) {
    size_t byte_length = 0u;
    if (!bounded_utf8_length(value, &byte_length) || byte_length == 0u ||
        byte_length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                                           static_cast<int>(byte_length), nullptr, 0);
    if (length <= 0)
        return {};
    std::vector<uint16_t> result(static_cast<size_t>(length) + 1u);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(byte_length),
                            reinterpret_cast<wchar_t*>(result.data()), length) != length) {
        return {};
    }
    result[static_cast<size_t>(length)] = 0u;
    return result;
}

sao_sdk_status_t SAO_SDK_CALL platform_tts_speak(void*, const char* text_utf8, float volume,
                                                 float rate) {
    try {
        if (!std::isfinite(volume) || !std::isfinite(rate))
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        const auto text = utf8_to_utf16(text_utf8);
        if (text.empty())
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        const int32_t native_rate = static_cast<int32_t>(std::clamp(rate, -10.0f, 10.0f));
        const int32_t native_volume = static_cast<int32_t>(std::clamp(volume, 0.0f, 1.0f) * 100.0f);
        return map_tts_status(
            sao_ui_alerts_speak(text.data(), nullptr, native_rate, native_volume));
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_tts_stop(void*) {
    try {
        return map_tts_status(sao_ui_alerts_stop());
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

void SAO_ENGINE_CALL platform_render_bridge_release(void* user_data) {
    delete static_cast<PlatformRenderBridge*>(user_data);
}

sao_status_t SAO_ENGINE_CALL platform_render_bridge_callback(
    int32_t hook_point, const SaoEngineRenderClockPayload* payload, void* user_data) {
    auto* bridge = static_cast<PlatformRenderBridge*>(user_data);
    if (bridge == nullptr || bridge->callback == nullptr || payload == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    SaoSdkRenderHookPayload sdk_payload{};
    sdk_payload.frame_time_us = payload->frame_time_us;
    sdk_payload.frame_index = payload->frame_index;
    sdk_payload.frame_delta_us = payload->frame_delta_us;
    sdk_payload.viewport_x_px = payload->viewport_x_px;
    sdk_payload.viewport_y_px = payload->viewport_y_px;
    sdk_payload.viewport_width_px = payload->viewport_width_px;
    sdk_payload.viewport_height_px = payload->viewport_height_px;
    sdk_payload.dispatch_flags = payload->flags;
    return static_cast<sao_status_t>(invoke_callback_barrier(
        [&] { return bridge->callback(hook_point, &sdk_payload, bridge->user_data); }));
}

sao_sdk_status_t SAO_SDK_CALL platform_render_register_ex(void*, const char* plugin_id_utf8,
                                                          const SaoSdkRenderHookSpec* spec,
                                                          sao_sdk_render_hook_callback_t callback,
                                                          void* callback_user_data,
                                                          uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' || spec == nullptr ||
        spec->surface_id_utf8 == nullptr || spec->surface_id_utf8[0] == '\0' ||
        callback == nullptr || out_provider_token == nullptr || !std::isfinite(spec->priority)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    auto* bridge = new (std::nothrow) PlatformRenderBridge();
    if (bridge == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    bridge->callback = callback;
    bridge->user_data = callback_user_data;
    sao_engine_hook_token_t token = 0;
    const sao_status_t status = sao_engine_render_clock_register(
        runtime.render_registry, plugin_id_utf8, spec->surface_id_utf8, spec->hook_point,
        spec->priority, platform_render_bridge_callback, bridge, platform_render_bridge_release,
        &token);
    if (status != SAO_STATUS_OK) {
        delete bridge;
        return static_cast<sao_sdk_status_t>(status);
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_render_register(void* user_data, const char* plugin_id_utf8,
                                                       int32_t hook_point,
                                                       sao_sdk_render_hook_callback_t callback,
                                                       void* callback_user_data,
                                                       uint64_t* out_provider_token) {
    const SaoSdkRenderHookSpec spec{SAO_ENGINE_ALL_SURFACES, hook_point, 0.0F};
    return platform_render_register_ex(user_data, plugin_id_utf8, &spec, callback,
                                       callback_user_data, out_provider_token);
}

sao_sdk_status_t SAO_SDK_CALL platform_render_unregister(void*, uint64_t provider_token) {
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    const sao_status_t status =
        sao_engine_render_clock_unregister(runtime.render_registry, provider_token);
    if (status == SAO_STATUS_ERR_SUBSCRIPTION_GONE) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    return static_cast<sao_sdk_status_t>(status);
}

sao_sdk_status_t SAO_SDK_CALL platform_render_request_redraw(void*, const char* surface_id_utf8) {
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    return static_cast<sao_sdk_status_t>(
        sao_engine_render_clock_request_redraw(runtime.render_registry, surface_id_utf8));
}

void SAO_CORE_CALL platform_timer_callback(void* user_data) {
    auto* entry = static_cast<PlatformTimerEntry*>(user_data);
    if (entry != nullptr && entry->callback != nullptr) {
        (void)invoke_void_callback_barrier([&] { entry->callback(0, entry->user_data); });
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_timer_register(void* user_data, uint32_t interval_ms,
                                                      sao_sdk_timer_callback_t callback,
                                                      void* callback_user_data,
                                                      uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (callback == nullptr || interval_ms == 0 || out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    auto entry = std::make_shared<PlatformTimerEntry>();
    entry->callback = callback;
    entry->user_data = callback_user_data;
    const sao_status_t status =
        sao_core_timer_create(interval_ms, platform_timer_callback, entry.get(), &entry->timer);
    if (status != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(status);
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (g_fail_next_platform_timer_insertion.exchange(false))
            throw std::bad_alloc{};
        const auto [it, inserted] = state->timers.emplace(token, entry);
        (void)it;
        if (!inserted) {
            sao_core_timer_destroy(entry->timer);
            return SAO_SDK_ERR_ALREADY_EXISTS;
        }
    } catch (...) {
        sao_core_timer_destroy(entry->timer);
        return SAO_SDK_ERR_INTERNAL;
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_timer_unregister(void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    std::shared_ptr<PlatformTimerEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->timers.find(provider_token);
        if (found == state->timers.end())
            return SAO_SDK_ERR_NOT_FOUND;
        entry = found->second;
    }
    sao_core_timer_destroy(entry->timer);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->timers.erase(provider_token);
    return SAO_SDK_OK;
}

void SAO_UI_CALL platform_hotkey_callback(const char*, const SaoUiInputEvent*, void* user_data) {
    auto* entry = static_cast<PlatformHotkeyEntry*>(user_data);
    if (entry == nullptr)
        return;
    CallbackActivityLease callback_lease(&entry->callback_activity);
    if (!callback_lease || entry->callback == nullptr)
        return;
    (void)invoke_void_callback_barrier([&] { entry->callback(0, entry->user_data); });
}

sao_sdk_status_t SAO_SDK_CALL platform_hotkey_register(void* user_data, const char* plugin_id_utf8,
                                                       const char* binding_id_utf8,
                                                       uint32_t virtual_key, uint32_t modifiers,
                                                       sao_sdk_hotkey_callback_t callback,
                                                       void* callback_user_data,
                                                       uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (plugin_id_utf8 == nullptr || binding_id_utf8 == nullptr || callback == nullptr ||
        out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto& runtime = SharedRuntime::instance();
    if (runtime.input_router == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    auto entry = std::make_shared<PlatformHotkeyEntry>();
    entry->callback = callback;
    entry->user_data = callback_user_data;
    SaoUiHotkeyBindingSpec spec{};
    spec.binding_id_utf8 = binding_id_utf8;
    spec.virtual_key = virtual_key;
    spec.modifiers = modifiers;
    spec.scope = SAO_UI_HOTKEY_SCOPE_GLOBAL;
    spec.enforce_ctrl_prefix =
        (modifiers & SAO_UI_MOD_CTRL_BIT) != 0 || (modifiers & SAO_UI_MOD_ANY_CTRL) != 0;
    const sao_status_t status =
        sao_ui_input_router_register_hotkey(runtime.input_router, plugin_id_utf8, &spec,
                                            platform_hotkey_callback, entry.get(), &entry->binding);
    if (status != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(status);
    try {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (g_fail_next_platform_hotkey_insertion.exchange(false))
            throw std::bad_alloc{};
        state->hotkeys.emplace(token, entry);
    } catch (...) {
        (void)sao_ui_input_router_unregister_hotkey(runtime.input_router, entry->binding);
        entry->callback_activity.retire_and_wait();
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_hotkey_unregister(void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    std::shared_ptr<PlatformHotkeyEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->hotkeys.find(provider_token);
        if (found == state->hotkeys.end())
            return SAO_SDK_ERR_NOT_FOUND;
        entry = found->second;
    }
    const sao_status_t status = sao_ui_input_router_unregister_hotkey(
        SharedRuntime::instance().input_router, entry->binding);
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_FOUND)
        return static_cast<sao_sdk_status_t>(status);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->hotkeys.erase(provider_token);
    }
    entry->callback_activity.retire_and_wait();
    return SAO_SDK_OK;
}

void SAO_UI_CALL platform_dialog_callback(SaoUiDialogButton pressed, const char* input_text_utf8,
                                          size_t input_text_len, void* user_data) {
    auto* entry = static_cast<PlatformDialogEntry*>(user_data);
    if (entry != nullptr && entry->callback != nullptr) {
        (void)invoke_void_callback_barrier([&] {
            entry->callback(0, static_cast<int32_t>(pressed), input_text_utf8, input_text_len,
                            entry->user_data);
        });
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_dialog_show(void* user_data, const char*,
                                                   const SaoSdkDialogSpec* spec,
                                                   sao_sdk_dialog_callback_t callback,
                                                   void* callback_user_data,
                                                   uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (spec == nullptr || out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    auto entry = std::make_shared<PlatformDialogEntry>();
    entry->callback = callback;
    entry->user_data = callback_user_data;
    sao_status_t status =
        sao_ui_dialog_create(SharedRuntime::instance().compositor, nullptr, &entry->dialog);
    if (status != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(status);
    SaoUiDialogSpec native{};
    native.kind = static_cast<SaoUiDialogKind>(spec->kind);
    native.title_utf8 = spec->title_utf8;
    native.message_utf8 = spec->message_utf8;
    native.input_prompt_utf8 = spec->input_prompt_utf8;
    native.input_default_utf8 = spec->input_default_utf8;
    native.input_max_length = spec->input_max_length;
    native.dismiss_on_focus_out = spec->dismiss_on_focus_out;
    native.dismiss_on_esc = spec->dismiss_on_esc;
    status = sao_ui_dialog_show(entry->dialog, &native, platform_dialog_callback, entry.get());
    if (status != SAO_STATUS_OK) {
        sao_ui_dialog_destroy(entry->dialog);
        return static_cast<sao_sdk_status_t>(status);
    }
    {
        try {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (g_fail_next_platform_dialog_insertion.exchange(false))
                throw std::bad_alloc{};
            const auto [it, inserted] = state->dialogs.emplace(token, entry);
            (void)it;
            if (!inserted)
                throw std::bad_alloc{};
        } catch (...) {
            sao_ui_dialog_destroy(entry->dialog);
            return SAO_SDK_ERR_INTERNAL;
        }
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_dialog_dismiss(void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    std::shared_ptr<PlatformDialogEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->dialogs.find(provider_token);
        if (found == state->dialogs.end())
            return SAO_SDK_ERR_NOT_FOUND;
        entry = found->second;
    }
    sao_ui_dialog_destroy(entry->dialog);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->dialogs.erase(provider_token);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_notify_show(void*, const char*, const SaoSdkNotifySpec* spec,
                                                   uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (spec == nullptr || spec->text_utf8 == nullptr || out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    const auto text = utf8_to_utf16(spec->text_utf8);
    if (text.empty())
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return static_cast<sao_sdk_status_t>(
        sao_ui_alerts_banner_show(text.data(), spec->duration_ms,
                                  static_cast<int32_t>(spec->argb_color), out_provider_token));
}

sao_sdk_status_t SAO_SDK_CALL platform_notify_dismiss(void*, uint64_t provider_token) {
    return static_cast<sao_sdk_status_t>(sao_ui_alerts_banner_hide(provider_token));
}

sao_sdk_status_t map_overlay_status(sao_status_t status) noexcept {
    if (status == SAO_STATUS_ERR_SURFACE_INVALID)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (status == SAO_STATUS_ERR_DEVICE_LOST)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    return map_tts_status(status);
}

thread_local PlatformOverlayEntry* g_overlay_callback_entry = nullptr;

class PlatformOverlayCallback {
  public:
    explicit PlatformOverlayCallback(PlatformOverlayEntry* entry)
        : lease_(&entry->callback_activity), previous_(g_overlay_callback_entry) {
        if (lease_)
            g_overlay_callback_entry = entry;
    }
    ~PlatformOverlayCallback() {
        g_overlay_callback_entry = previous_;
    }
    explicit operator bool() const noexcept {
        return static_cast<bool>(lease_);
    }

  private:
    CallbackActivityLease lease_;
    PlatformOverlayEntry* previous_;
};

class PlatformOverlayOperation {
  public:
    explicit PlatformOverlayOperation(PlatformProviderState* state) : state_(state) {
        if (g_overlay_callback_entry != nullptr)
            return;
        std::lock_guard lock(state_->mutex);
        active_ = !state_->overlay_operation_active;
        if (active_)
            state_->overlay_operation_active = true;
    }
    ~PlatformOverlayOperation() {
        if (active_) {
            std::lock_guard lock(state_->mutex);
            state_->overlay_operation_active = false;
        }
    }
    explicit operator bool() const noexcept {
        return active_;
    }

  private:
    PlatformProviderState* state_;
    bool active_ = false;
};

sao_sdk_status_t overlay_document(const SaoSdkOverlaySpec& spec, PlatformOverlayEntry& entry,
                                  std::string& panel_spec) {
    using Json = nlohmann::json;
    constexpr size_t maximum_bytes = 8U << 20U;
    if (spec.spec_json_utf8 == nullptr || spec.spec_len == 0 || spec.spec_len > maximum_bytes)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    try {
        size_t events = 0;
        const auto bounded = [&events](int depth, Json::parse_event_t, Json& value) {
            if (depth > 32 || ++events > 65536 ||
                (value.is_string() &&
                 value.get_ref<const std::string&>().find('\0') != std::string::npos))
                throw std::invalid_argument("overlay bounds");
            return true;
        };
        Json document =
            Json::parse(spec.spec_json_utf8, spec.spec_json_utf8 + spec.spec_len, bounded);
        if (document.is_object() && document.contains("spec"))
            document = Json(document["spec"]);
        if (document.is_object() && document.contains("root"))
            document = Json(document["root"]);
        Json nodes = Json::array();
        if (document.is_object() && document.contains("canvas")) {
            Json node = document["canvas"];
            if (!node.is_object())
                return SAO_SDK_ERR_INVALID_ARGUMENT;
            if (!node.contains("type"))
                node["type"] = "canvas";
            nodes.push_back(std::move(node));
        } else if (document.is_object() && document.contains("nodes")) {
            nodes = document["nodes"];
        } else {
            nodes.push_back(std::move(document));
        }
        if (!nodes.is_array() || nodes.empty() || nodes.size() > 64)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        struct node_geometry {
            int32_t x = 0;
            int32_t y = 0;
            int32_t z = 0;
            int32_t width = 0;
            int32_t height = 0;
        };
        std::vector<node_geometry> geometries;
        geometries.reserve(nodes.size());
        int32_t minimum_x = (std::numeric_limits<int32_t>::max)();
        int32_t minimum_y = (std::numeric_limits<int32_t>::max)();
        int32_t minimum_z = (std::numeric_limits<int32_t>::max)();
        int32_t maximum_x = (std::numeric_limits<int32_t>::min)();
        int32_t maximum_y = (std::numeric_limits<int32_t>::min)();
        bool draggable = false;
        bool rect_hit = false;
        for (size_t index = 0; index < nodes.size(); ++index) {
            auto& node = nodes[index];
            if (!node.is_object())
                return SAO_SDK_ERR_INVALID_ARGUMENT;
            const auto kind = node.value("type", std::string{});
            if (kind != "canvas" && kind != "rgba_frame")
                return SAO_SDK_ERR_UNSUPPORTED;
            if (const auto ops = node.find("ops"); ops != node.end()) {
                if (kind != "canvas" || !ops->is_array() || ops->size() > 4000U)
                    return SAO_SDK_ERR_INVALID_ARGUMENT;
                for (const auto& operation : *ops) {
                    if (!operation.is_object())
                        return SAO_SDK_ERR_INVALID_ARGUMENT;
                    const auto action = operation.find("op");
                    if (action == operation.end() || !action->is_string())
                        return SAO_SDK_ERR_INVALID_ARGUMENT;
                    std::string name = action->get<std::string>();
                    const auto first = name.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos)
                        return SAO_SDK_ERR_INVALID_ARGUMENT;
                    name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1U);
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                    if (name != "line" && name != "rect" && name != "oval" && name != "polygon" &&
                        name != "text" && name != "ctext")
                        return SAO_SDK_ERR_INVALID_ARGUMENT;
                    if (name == "rect" || name == "oval") {
                        for (const auto* dimension : {"w", "h"}) {
                            const auto value = operation.find(dimension);
                            if (value == operation.end())
                                continue;
                            if (!value->is_number())
                                return SAO_SDK_ERR_INVALID_ARGUMENT;
                            const double number = value->get<double>();
                            if (!std::isfinite(number) || number < 0 || number > 4096)
                                return SAO_SDK_ERR_INVALID_ARGUMENT;
                        }
                    }
                }
            }
            if (node.contains("id") && !node["id"].is_string())
                return SAO_SDK_ERR_INVALID_ARGUMENT;
            if (!node.contains("id"))
                node["id"] = "sdk.overlay.node." + std::to_string(index);
            for (const auto& [short_key, long_key] :
                 {std::pair{"w", "width"}, std::pair{"h", "height"}}) {
                if (!node.contains(short_key))
                    continue;
                if (node.contains(long_key) && node[short_key] != node[long_key])
                    return SAO_SDK_ERR_INVALID_ARGUMENT;
                node[long_key] = node[short_key];
                node.erase(short_key);
            }
            const auto geometry = [&node](const char* key, int fallback, int minimum, int maximum,
                                          int32_t& output) {
                const auto value = node.find(key);
                if (value == node.end()) {
                    output = fallback;
                    return true;
                }
                if (!value->is_number())
                    return false;
                const double number = value->get<double>();
                if (!std::isfinite(number) || number < minimum || number > maximum)
                    return false;
                output = static_cast<int32_t>(std::nearbyint(number));
                return true;
            };
            node_geometry geometry_value;
            if (!geometry("width", 320, 1, 4096, geometry_value.width) ||
                !geometry("height", kind == "canvas" ? 160 : 480, 1, 4096, geometry_value.height) ||
                !geometry("x", 0, -32768, 32768, geometry_value.x) ||
                !geometry("y", 0, -32768, 32768, geometry_value.y) ||
                !geometry("z", 0, -10000, 10000, geometry_value.z)) {
                return SAO_SDK_ERR_INVALID_ARGUMENT;
            }
            if (kind == "rgba_frame") {
                for (const char* key : {"width", "height"}) {
                    const auto value = node.find(key);
                    if (value != node.end() &&
                        (!value->is_number() ||
                         value->get<double>() != std::nearbyint(value->get<double>()))) {
                        return SAO_SDK_ERR_INVALID_ARGUMENT;
                    }
                }
            }
            if (node.contains("draggable") && !node["draggable"].is_boolean())
                return SAO_SDK_ERR_INVALID_ARGUMENT;
            bool node_draggable = node.value("draggable", kind == "rgba_frame");
            const auto hit_test =
                node.value("hit_test", std::string(kind == "rgba_frame" ? "rect" : "alpha"));
            if (hit_test != "alpha" && hit_test != "rect" && hit_test != "none")
                return SAO_SDK_ERR_INVALID_ARGUMENT;
            if (hit_test == "none")
                node_draggable = false;
            draggable = draggable || node_draggable;
            rect_hit = rect_hit || (node_draggable && hit_test == "rect");
            node["width"] = geometry_value.width;
            node["height"] = geometry_value.height;
            node["x"] = geometry_value.x;
            node["y"] = geometry_value.y;
            node["z"] = geometry_value.z;
            node["draggable"] = node_draggable;
            node["hit_test"] = hit_test;
            const char* background = kind == "canvas" ? "bg" : "background";
            if (!node.contains(background) ||
                (node[background].is_string() &&
                 node[background].get_ref<const std::string&>().find_first_not_of(" \t\r\n") ==
                     std::string::npos)) {
                node[background] = "transparent";
            }
            minimum_x = (std::min)(minimum_x, geometry_value.x);
            minimum_y = (std::min)(minimum_y, geometry_value.y);
            minimum_z = (std::min)(minimum_z, geometry_value.z);
            maximum_x = (std::max)(maximum_x, geometry_value.x + geometry_value.width);
            maximum_y = (std::max)(maximum_y, geometry_value.y + geometry_value.height);
            geometries.push_back(geometry_value);
        }
        const int64_t union_width = static_cast<int64_t>(maximum_x) - minimum_x;
        const int64_t union_height = static_cast<int64_t>(maximum_y) - minimum_y;
        if (union_width <= 0 || union_width > 4096 || union_height <= 0 || union_height > 4096)
            return SAO_SDK_ERR_INVALID_ARGUMENT;
        entry.x = minimum_x;
        entry.y = minimum_y;
        entry.z = minimum_z;
        entry.width = static_cast<int32_t>(union_width);
        entry.height = static_cast<int32_t>(union_height);
        entry.draggable = draggable;
        entry.rect_hit = rect_hit;
        Json engine_nodes = nodes;
        entry.paint_nodes.clear();
        entry.paint_nodes.reserve(nodes.size());
        for (size_t index = 0; index < nodes.size(); ++index) {
            const std::string panel_id = "sdk.overlay.node." + std::to_string(index);
            nodes[index]["id"] = panel_id;
            nodes[index]["x"] = geometries[index].x - minimum_x;
            nodes[index]["y"] = geometries[index].y - minimum_y;
            nodes[index]["z"] = 0;
            entry.paint_nodes.push_back({panel_id, geometries[index].x - minimum_x,
                                         geometries[index].y - minimum_y, geometries[index].z,
                                         geometries[index].width, geometries[index].height});
        }
        std::stable_sort(entry.paint_nodes.begin(), entry.paint_nodes.end(),
                         [](const auto& left, const auto& right) { return left.z < right.z; });
        const Json engine_spec{
            {"version", SAO_UI_SPEC_VERSION}, {"layout", "absolute"}, {"nodes", engine_nodes}};
        const Json panel_document{
            {"version", SAO_UI_SPEC_VERSION}, {"layout", "absolute"}, {"nodes", nodes}};
        entry.engine_spec = Json{{"spec", engine_spec}}.dump();
        panel_spec = panel_document.dump();
        return panel_spec.size() <= maximum_bytes ? SAO_SDK_OK : SAO_SDK_ERR_INVALID_ARGUMENT;
    } catch (const std::bad_alloc&) {
        return SAO_SDK_ERR_INTERNAL;
    } catch (...) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
}

void SAO_UI_CALL overlay_cursor(float x, float y, void* user_data) {
    auto* entry = static_cast<PlatformOverlayEntry*>(user_data);
    PlatformOverlayCallback callback_lease(entry);
    if (!callback_lease || !entry->registry_owned || !entry->dragging || !std::isfinite(x) ||
        !std::isfinite(y))
        return;
    const int32_t next_x = static_cast<int32_t>(std::clamp(
        std::nearbyint(static_cast<double>(entry->x) + x - entry->drag_x), -32768.0, 32768.0));
    const int32_t next_y = static_cast<int32_t>(std::clamp(
        std::nearbyint(static_cast<double>(entry->y) + y - entry->drag_y), -32768.0, 32768.0));
    if (sao_ui_layer_set_position(entry->layer, next_x, next_y) == SAO_STATUS_OK) {
        entry->x = next_x;
        entry->y = next_y;
    }
}

void SAO_UI_CALL overlay_leave(void* user_data) {
    auto* entry = static_cast<PlatformOverlayEntry*>(user_data);
    PlatformOverlayCallback callback_lease(entry);
    if (callback_lease)
        entry->dragging = false;
}

void SAO_UI_CALL overlay_button(int32_t button, int32_t action, int32_t, float x, float y,
                                void* user_data) {
    auto* entry = static_cast<PlatformOverlayEntry*>(user_data);
    PlatformOverlayCallback callback_lease(entry);
    if (!callback_lease || !entry->registry_owned || !entry->draggable || button != 0)
        return;
    entry->dragging = action == 1;
    entry->drag_x = x;
    entry->drag_y = y;
}

sao_sdk_status_t destroy_overlay_native(PlatformOverlayEntry& entry) {
    if (entry.layer != nullptr) {
        auto status = sao_ui_layer_set_visible(entry.layer, false);
        if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_HANDLE_INVALID)
            return map_overlay_status(status);
        if (status == SAO_STATUS_OK) {
            status = sao_ui_layer_set_input_callbacks(entry.layer, nullptr, nullptr, nullptr,
                                                      nullptr, nullptr);
            if (status != SAO_STATUS_OK)
                return map_overlay_status(status);
            entry.callback_activity.retire_and_wait();
            sao_ui_layer_destroy(entry.layer);
            status = sao_ui_layer_request_redraw(entry.layer);
            if (status != SAO_STATUS_ERR_HANDLE_INVALID)
                return status == SAO_STATUS_OK ? SAO_SDK_ERR_BUSY : map_overlay_status(status);
        }
        entry.layer = nullptr;
    }
    entry.callback_activity.retire_and_wait();
    if (entry.panel != nullptr) {
        sao_ui_panel_destroy(entry.panel);
        SaoPanelState state{};
        const auto status = sao_ui_panel_get_state(entry.panel, &state);
        if (status != SAO_STATUS_ERR_HANDLE_INVALID)
            return status == SAO_STATUS_OK ? SAO_SDK_ERR_BUSY : map_overlay_status(status);
        entry.panel = nullptr;
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t paint_overlay_native(PlatformOverlayEntry& entry, const std::string& panel_spec,
                                      uint64_t token) {
    const auto name = "sdk.overlay." + std::to_string(token);
    SaoPanelConfig config{};
    config.panel_id_utf8 = name.c_str();
    config.default_width = entry.width;
    config.default_height = entry.height;
    config.min_width = config.min_height = 1;
    config.rendering_mode = SAO_UI_PANEL_RENDER_NATIVE;
    // The headless panel owns the shared canvas parser; its chrome is never composited.
    sao_status_t status = SAO_STATUS_OK;
    if (entry.panel == nullptr) {
        status = sao_ui_panel_create(nullptr, &config, &entry.panel);
        if (status != SAO_STATUS_OK)
            return map_overlay_status(status);
    }
    status = sao_ui_panel_set_spec(entry.panel, reinterpret_cast<const uint8_t*>(panel_spec.data()),
                                   panel_spec.size());
    if (status != SAO_STATUS_OK)
        return map_overlay_status(status);
    entry.panel_spec = panel_spec;
    std::vector<sao_ui_widget_handle_t> widgets;
    widgets.reserve(entry.paint_nodes.size());
    for (const auto& node : entry.paint_nodes) {
        sao_ui_widget_handle_t widget = nullptr;
        status = sao_ui_panel_find_widget(entry.panel, node.panel_id.c_str(), &widget);
        if (status != SAO_STATUS_OK)
            return map_overlay_status(status);
        widgets.push_back(widget);
    }
    SaoUiOffscreenRasterDesc desc{static_cast<uint32_t>(entry.width),
                                  static_cast<uint32_t>(entry.height), 0};
    sao_ui_offscreen_raster_handle_t raster = nullptr;
    status = sao_ui_offscreen_raster_create(&desc, &raster);
    if (status != SAO_STATUS_OK)
        return map_overlay_status(status);
    const std::unique_ptr<sao_ui_offscreen_raster_s, decltype(&sao_ui_offscreen_raster_destroy)>
        raster_owner(raster, sao_ui_offscreen_raster_destroy);
    sao_ui_paint_ctx_handle_t paint = nullptr;
    status = sao_ui_paint_ctx_create_offscreen(raster, &paint);
    if (status != SAO_STATUS_OK)
        return map_overlay_status(status);
    const std::unique_ptr<sao_ui_paint_ctx_s, decltype(&sao_ui_paint_ctx_destroy)> paint_owner(
        paint, sao_ui_paint_ctx_destroy);
    status = sao_ui_paint_ctx_begin_frame(paint);
    if (status == SAO_STATUS_OK) {
        for (size_t index = 0; index < widgets.size() && status == SAO_STATUS_OK; ++index) {
            const auto& node = entry.paint_nodes[index];
            status = sao_ui_widget_paint(widgets[index], paint, static_cast<float>(node.x),
                                         static_cast<float>(node.y), static_cast<float>(node.width),
                                         static_cast<float>(node.height));
        }
        const auto end_status = sao_ui_paint_ctx_end_frame(paint);
        if (status == SAO_STATUS_OK)
            status = end_status;
    }
    if (status != SAO_STATUS_OK)
        return map_overlay_status(status);
    std::vector<uint8_t> pixels(static_cast<size_t>(entry.width) * entry.height * 4U);
    size_t written = 0;
    uint32_t width = 0, height = 0, stride = 0;
    status = sao_ui_offscreen_raster_snapshot(raster, pixels.data(), pixels.size(), &written,
                                              &width, &height, &stride);
    if (status != SAO_STATUS_OK)
        return map_overlay_status(status);
    if (width != desc.width_px || height != desc.height_px || stride != width * 4U ||
        written != pixels.size())
        return SAO_SDK_ERR_INTERNAL;
    SaoLayerConfig layer{};
    layer.struct_size = sizeof(layer);
    layer.name_utf8 = name.c_str();
    layer.x = entry.x;
    layer.y = entry.y;
    layer.width = entry.width;
    layer.height = entry.height;
    layer.z_order = entry.z;
    layer.click_through = !entry.draggable;
    layer.rect_hit = entry.rect_hit;
    layer.bgra_swizzle = true;
    status = sao_ui_layer_create(entry.compositor, &layer, &entry.layer);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_set_visible(entry.layer, false);
    if (status == SAO_STATUS_OK)
        status = sao_ui_layer_update_bgra(entry.layer, pixels.data(), width, height, stride);
    if (status == SAO_STATUS_OK && entry.draggable)
        status = sao_ui_layer_set_input_callbacks(entry.layer, overlay_cursor, overlay_leave,
                                                  overlay_button, nullptr, &entry);
    return map_overlay_status(status);
}

sao_sdk_status_t SAO_SDK_CALL platform_overlay_set(void* user_data, const char* plugin_id_utf8,
                                                   const SaoSdkOverlaySpec* spec,
                                                   uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (user_data == nullptr || plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0' ||
        spec == nullptr || spec->surface_id_utf8 == nullptr || spec->surface_id_utf8[0] == '\0' ||
        out_provider_token == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto& runtime = SharedRuntime::instance();
    sao_ui_compositor_handle_t compositor = nullptr;
    auto result = runtime.get_bound_compositor(&compositor);
    if (result != SAO_SDK_OK)
        return result;
    result = map_overlay_status(sao_ui_compositor_require_owner_thread(compositor));
    if (result != SAO_SDK_OK)
        return result;
    if (runtime.render_registry == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    auto* state = static_cast<PlatformProviderState*>(user_data);
    PlatformOverlayOperation operation(state);
    if (!operation)
        return SAO_SDK_ERR_BUSY;
    auto entry = std::make_shared<PlatformOverlayEntry>();
    entry->plugin_id = plugin_id_utf8;
    entry->surface_id = spec->surface_id_utf8;
    entry->compositor = compositor;
    std::string panel_spec;
    result = overlay_document(*spec, *entry, panel_spec);
    if (result != SAO_SDK_OK)
        return result;
    const uint64_t token = state->next_token.fetch_add(1);
    if (token == 0)
        return SAO_SDK_ERR_INTERNAL;
    std::shared_ptr<PlatformOverlayEntry> previous;
    {
        std::lock_guard lock(state->mutex);
        for (const auto& [unused, candidate] : state->overlays) {
            (void)unused;
            if (candidate->registry_owned && candidate->plugin_id == entry->plugin_id &&
                candidate->surface_id == entry->surface_id)
                previous = candidate;
        }
        if (g_fail_next_platform_overlay_insertion.exchange(false))
            return SAO_SDK_ERR_INTERNAL;
        if (!state->overlays.emplace(token, entry).second)
            return SAO_SDK_ERR_INTERNAL;
    }
    const bool reused_panel = previous != nullptr && previous->panel != nullptr &&
                              previous->width == entry->width && previous->height == entry->height;
    if (reused_panel)
        entry->panel = std::exchange(previous->panel, nullptr);
    bool previous_hidden = false;
    const auto abandon = [&](sao_sdk_status_t failure) {
        if (previous_hidden) {
            const auto restore =
                map_overlay_status(sao_ui_layer_set_visible(previous->layer, true));
            if (restore != SAO_SDK_OK)
                failure = restore;
            previous_hidden = false;
        }
        sao_sdk_status_t panel_restore = SAO_SDK_OK;
        if (reused_panel && previous->panel == nullptr) {
            const bool candidate_updated_panel = !entry->panel_spec.empty() ||
                                                 failure != SAO_SDK_ERR_INVALID_ARGUMENT;
            previous->panel = std::exchange(entry->panel, nullptr);
            if (candidate_updated_panel && previous->panel != nullptr &&
                !previous->panel_spec.empty()) {
                panel_restore = map_overlay_status(sao_ui_panel_set_spec(
                    previous->panel, reinterpret_cast<const uint8_t*>(previous->panel_spec.data()),
                    previous->panel_spec.size()));
            }
        }
        const auto cleanup = destroy_overlay_native(*entry);
        if (cleanup != SAO_SDK_OK) {
            *out_provider_token = token;
            return cleanup;
        }
        std::lock_guard lock(state->mutex);
        state->overlays.erase(token);
        return panel_restore == SAO_SDK_OK ? failure : panel_restore;
    };
    try {
        result = paint_overlay_native(*entry, panel_spec, token);
        if (result != SAO_SDK_OK)
            return abandon(result);
        result = map_overlay_status(sao_ui_layer_set_visible(entry->layer, true));
        if (result != SAO_SDK_OK)
            return abandon(result);
        if (previous != nullptr && previous->layer != nullptr) {
            previous_hidden = true;
            result = map_overlay_status(sao_ui_layer_set_visible(previous->layer, false));
            if (result != SAO_SDK_OK)
                return abandon(result);
        }
        result = map_overlay_status(sao_engine_render_hook_set_overlay(
            runtime.render_registry, plugin_id_utf8, spec->surface_id_utf8,
            reinterpret_cast<const uint8_t*>(entry->engine_spec.data()),
            entry->engine_spec.size()));
        if (result != SAO_SDK_OK)
            return abandon(result);
        entry->registry_owned = true;
        if (previous != nullptr)
            previous->registry_owned = false;
        *out_provider_token = token;
        return previous != nullptr ? destroy_overlay_native(*previous) : SAO_SDK_OK;
    } catch (...) {
        return entry->registry_owned ? SAO_SDK_ERR_INTERNAL : abandon(SAO_SDK_ERR_INTERNAL);
    }
}

sao_sdk_status_t SAO_SDK_CALL platform_overlay_clear(void* user_data, uint64_t provider_token) {
    if (user_data == nullptr || provider_token == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = static_cast<PlatformProviderState*>(user_data);
    PlatformOverlayOperation operation(state);
    if (!operation)
        return SAO_SDK_ERR_BUSY;
    std::shared_ptr<PlatformOverlayEntry> entry;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->overlays.find(provider_token);
        if (found == state->overlays.end())
            return SAO_SDK_ERR_NOT_FOUND;
        entry = found->second;
    }
    auto status = map_overlay_status(sao_ui_compositor_require_owner_thread(entry->compositor));
    if (status != SAO_SDK_OK)
        return status;
    status = destroy_overlay_native(*entry);
    if (status != SAO_SDK_OK)
        return status;
    if (entry->registry_owned) {
        status = map_overlay_status(sao_engine_render_hook_clear_overlay(
            SharedRuntime::instance().render_registry, entry->plugin_id.c_str(),
            entry->surface_id.c_str()));
        if (status != SAO_SDK_OK)
            return status;
        entry->registry_owned = false;
    }
    std::lock_guard lock(state->mutex);
    state->overlays.erase(provider_token);
    return SAO_SDK_OK;
}

const SaoSdkProviderVTable* platform_provider() {
    static const SaoSdkProviderVTable provider = {
        SAO_SDK_PROVIDER_ABI_VERSION,
        sizeof(SaoSdkProviderVTable),
        &platform_provider_state(),
        platform_provider_retain,
        platform_provider_release,
        platform_tts_speak,
        platform_tts_stop,
        platform_render_register,
        platform_render_unregister,
        platform_timer_register,
        platform_timer_unregister,
        platform_hotkey_register,
        platform_hotkey_unregister,
        platform_dialog_show,
        platform_dialog_dismiss,
        platform_notify_show,
        platform_notify_dismiss,
        platform_overlay_set,
        platform_overlay_clear,
        platform_render_register_ex,
        platform_render_request_redraw,
        platform_gpu_hunt_open_session,
        platform_gpu_hunt_close_session,
        platform_gpu_hunt_attach,
        platform_gpu_hunt_detach,
        platform_gpu_hunt_enum_regions,
        platform_gpu_hunt_read,
    };
    return &provider;
}

} // namespace

bool provider_callback_reentered(ContextState* state) noexcept {
    return state != nullptr && g_provider_callback_owner == state;
}

#if defined(SAO_SDK_TESTING)
void test_fail_next_platform_timer_insertion() noexcept {
    g_fail_next_platform_timer_insertion.store(true);
}

void test_fail_next_platform_hotkey_insertion() noexcept {
    g_fail_next_platform_hotkey_insertion.store(true);
}

void test_fail_next_platform_dialog_insertion() noexcept {
    g_fail_next_platform_dialog_insertion.store(true);
}

void test_fail_next_platform_overlay_insertion() noexcept {
    g_fail_next_platform_overlay_insertion.store(true);
}

void test_fail_next_render_state_insertion() noexcept {
    g_fail_next_render_state_insertion.store(true);
}

void test_fail_next_hotkey_state_insertion() noexcept {
    g_fail_next_hotkey_state_insertion.store(true);
}

void test_fail_next_notify_state_insertion() noexcept {
    g_fail_next_notify_state_insertion.store(true);
}

void test_fail_next_overlay_state_insertion() noexcept {
    g_fail_next_overlay_state_insertion.store(true);
}
#endif

sao_sdk_status_t bind_provider(ContextState* state, const SaoSdkProviderVTable* provider) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (state->provider_bind_transaction.load(std::memory_order_acquire)) {
        return SAO_SDK_ERR_BUSY;
    }
    if (state->destroying.load(std::memory_order_acquire) ||
        state->destroy_quarantined.load(std::memory_order_acquire))
        return SAO_SDK_ERR_BUSY;
    if (provider_callback_reentered(state) || plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    std::lock_guard<std::recursive_mutex> lifecycle_lock(state->provider_lifecycle_mutex);
    if (provider == nullptr)
        return provider_cleanup(state);
    if ((provider->abi_version >> 16) != SAO_SDK_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < kProviderMinimumSize) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }

    SaoSdkProviderVTable copy{};
    std::memcpy(&copy, provider, std::min<size_t>(provider->struct_size, sizeof(copy)));
    if ((copy.retain == nullptr) != (copy.release == nullptr))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    bool candidate_retained = false;
    if (copy.retain != nullptr) {
        const auto retain_status = invoke_provider_callback(state, [&] {
            copy.retain(copy.user_data);
            return SAO_SDK_OK;
        });
        if (retain_status != SAO_SDK_OK)
            return retain_status;
        candidate_retained = true;
    }

    const auto cleanup_status = provider_cleanup(state);
    if (cleanup_status != SAO_SDK_OK) {
        if (candidate_retained && copy.release != nullptr) {
            const auto release_status = invoke_provider_callback(state, [&] {
                copy.release(copy.user_data);
                return SAO_SDK_OK;
            });
            if (release_status != SAO_SDK_OK) {
                std::lock_guard<std::mutex> lock(state->mu);
                state->provider_release_quarantine.push_back(copy);
            }
        }
        return cleanup_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->provider_mutex);
        state->provider_accepting = true;
        state->provider_cleanup_status = SAO_SDK_OK;
        state->provider_retained = candidate_retained;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->provider = copy;
        state->provider_bound = true;
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t bind_platform_provider(ContextState* state) {
    const sao_sdk_status_t runtime_status = SharedRuntime::instance().ensure_started();
    if (runtime_status != SAO_SDK_OK)
        return runtime_status;
    return bind_provider(state, platform_provider());
}

sao_sdk_status_t prepare_platform_provider_binding(ContextState* state,
                                                   ProviderBindingCandidate* out_candidate) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (out_candidate == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_candidate = {};
    const sao_sdk_status_t runtime_status = SharedRuntime::instance().ensure_started();
    if (runtime_status != SAO_SDK_OK)
        return runtime_status;
    out_candidate->provider = *platform_provider();
    if (out_candidate->provider.retain == nullptr)
        return SAO_SDK_OK;
    const auto status = invoke_provider_callback(state, [&] {
        out_candidate->provider.retain(out_candidate->provider.user_data);
        return SAO_SDK_OK;
    });
    if (status != SAO_SDK_OK)
        return status;
    out_candidate->retained = true;
    return SAO_SDK_OK;
}

sao_sdk_status_t discard_provider_binding_candidate(ContextState* state,
                                                    ProviderBindingCandidate* candidate) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (candidate == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (!candidate->retained || candidate->provider.release == nullptr) {
        *candidate = {};
        return SAO_SDK_OK;
    }
    const auto status = invoke_provider_callback(state, [&] {
        candidate->provider.release(candidate->provider.user_data);
        return SAO_SDK_OK;
    });
    if (status != SAO_SDK_OK) {
        try {
            std::lock_guard<std::mutex> lock(state->mu);
            state->provider_release_quarantine.push_back(candidate->provider);
        } catch (...) {
            return SAO_SDK_ERR_INTERNAL;
        }
    }
    *candidate = {};
    return status;
}

bool platform_provider_bound(const ContextState* state) {
    if (state == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(const_cast<ContextState*>(state)->mu);
    return state->provider_bound && state->provider.user_data == platform_provider()->user_data;
}

sao_sdk_status_t provider_status(const ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    bool bound = false;
    {
        std::lock_guard<std::mutex> lock(const_cast<ContextState*>(state)->mu);
        bound = state->provider_bound;
    }
    if (!bound)
        return SAO_SDK_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(const_cast<ContextState*>(state)->provider_mutex);
    if (state->provider_cleanup_status != SAO_SDK_OK)
        return state->provider_cleanup_status;
    return state->provider_accepting ? SAO_SDK_OK : SAO_SDK_ERR_BUSY;
}

sao_sdk_status_t provider_cleanup(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (provider_callback_reentered(state) || plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    std::lock_guard<std::recursive_mutex> lifecycle_lock(state->provider_lifecycle_mutex);

    std::vector<SaoSdkProviderVTable> release_quarantine;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        release_quarantine = state->provider_release_quarantine;
    }
    for (const auto& provider : release_quarantine) {
        const auto status =
            provider.release == nullptr ? SAO_SDK_OK : invoke_provider_callback(state, [&] {
                provider.release(provider.user_data);
                return SAO_SDK_OK;
            });
        if (status != SAO_SDK_OK)
            return status;
        std::lock_guard<std::mutex> lock(state->mu);
        const auto found = std::find_if(
            state->provider_release_quarantine.begin(), state->provider_release_quarantine.end(),
            [&provider](const auto& item) {
                return item.user_data == provider.user_data && item.release == provider.release;
            });
        if (found != state->provider_release_quarantine.end())
            state->provider_release_quarantine.erase(found);
    }

    SaoSdkProviderVTable provider{};
    bool was_bound = false;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        provider = state->provider;
        was_bound = state->provider_bound;
    }
    if (!was_bound)
        return SAO_SDK_OK;
    {
        std::unique_lock<std::mutex> lock(state->provider_mutex);
        state->provider_accepting = false;
        state->provider_idle.wait(lock, [state] { return state->provider_active_calls == 0; });
    }

    for (;;) {
        CapabilityRegistration registration;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            if (state->capability_registrations.empty())
                break;
            registration = state->capability_registrations.back();
        }
        const auto status =
            normalize_unregister_status(unregister_provider_token(state, provider, registration));
        if (status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->provider_mutex);
            state->provider_cleanup_status = status;
            return status;
        }
        finish_registration(state, registration);
    }

    bool retained = false;
    {
        std::lock_guard<std::mutex> lock(state->provider_mutex);
        retained = state->provider_retained;
    }
    if (retained && provider.release != nullptr) {
        const auto release_status = invoke_provider_callback(state, [&] {
            provider.release(provider.user_data);
            return SAO_SDK_OK;
        });
        if (release_status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->provider_mutex);
            state->provider_cleanup_status = release_status;
            return release_status;
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->provider_mutex);
        state->provider_retained = false;
        state->provider_cleanup_status = SAO_SDK_OK;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->provider = {};
        state->provider_bound = false;
        state->render_hooks.clear();
        state->hotkeys.clear();
        state->overlays.clear();
        state->banner_ids.clear();
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t normalize_provider_status(sao_sdk_status_t status) {
    if (status == SAO_STATUS_ERR_CAPABILITY_MISSING || status == SAO_STATUS_ERR_NOT_IMPLEMENTED) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    return status;
}

sao_sdk_status_t configure_memory_provider(ContextState* state,
                                           const SaoSdkMemoryProviderVTable* provider,
                                           ProviderConfigureOrigin origin) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (origin == ProviderConfigureOrigin::cleanup && provider != nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (state->destroying.load(std::memory_order_acquire) &&
        origin != ProviderConfigureOrigin::cleanup)
        return SAO_SDK_ERR_BUSY;
    if (memory_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;

    std::lock_guard<std::mutex> lifecycle_lock(state->memory_lifecycle_mutex);

    std::vector<std::shared_ptr<MemoryProviderSession>> quarantined;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        quarantined = state->memory_quarantine;
    }
    for (const auto& session : quarantined) {
        const auto status = shutdown_memory_session(session);
        if (status != SAO_SDK_OK)
            return status;
        std::lock_guard<std::mutex> lock(state->mu);
        std::erase(state->memory_quarantine, session);
    }

    std::shared_ptr<MemoryProviderSession> previous;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        previous = state->memory_provider;
    }
    if (provider == nullptr) {
        const auto cleanup_status = shutdown_memory_session(previous);
        if (cleanup_status != SAO_SDK_OK)
            return cleanup_status;
        std::lock_guard<std::mutex> lock(state->mu);
        if (state->memory_provider == previous)
            state->memory_provider.reset();
        return SAO_SDK_OK;
    }
    if ((provider->abi_version >> 16) != SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MAJOR ||
        provider->struct_size < kMemoryProviderMinimumSize) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }

    SaoSdkMemoryProviderVTable copy{};
    std::memcpy(&copy, provider, std::min<size_t>(provider->struct_size, sizeof(copy)));
    if (copy.retain == nullptr || copy.release == nullptr || copy.open_session == nullptr ||
        copy.close_session == nullptr || copy.attach == nullptr || copy.detach == nullptr ||
        copy.enumerate_modules == nullptr || copy.read == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    std::shared_ptr<MemoryProviderSession> candidate;
    try {
        candidate = std::make_shared<MemoryProviderSession>();
        candidate->owner = state;
        candidate->provider = copy;
        const auto retain_status = invoke_callback_barrier([&] {
            MemoryCallbackScope callback_scope(state, candidate.get());
            copy.retain(copy.user_data);
            return SAO_SDK_OK;
        });
        if (retain_status != SAO_SDK_OK)
            return retain_status;
        candidate->retained = true;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }

    const auto open_status = invoke_callback_barrier([&] {
        MemoryCallbackScope callback_scope(state, candidate.get());
        return normalize_provider_status(
            copy.open_session(copy.user_data, state->plugin_id.c_str(), &candidate->session));
    });
    if (open_status != SAO_SDK_OK || candidate->session == nullptr) {
        candidate->accepting = false;
        const auto cleanup_status = shutdown_memory_session(candidate);
        if (cleanup_status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->memory_quarantine.push_back(candidate);
            return cleanup_status;
        }
        return open_status == SAO_SDK_OK ? SAO_SDK_ERR_HANDLE_INVALID : open_status;
    }

    const auto cleanup_status = shutdown_memory_session(previous);
    if (cleanup_status != SAO_SDK_OK) {
        candidate->accepting = false;
        const auto candidate_cleanup_status = shutdown_memory_session(candidate);
        if (candidate_cleanup_status != SAO_SDK_OK) {
            std::lock_guard<std::mutex> lock(state->mu);
            state->memory_quarantine.push_back(candidate);
        }
        return cleanup_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->memory_provider = std::move(candidate);
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t memory_provider_status(const ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    std::shared_ptr<MemoryProviderSession> session;
    {
        std::lock_guard<std::mutex> lock(const_cast<ContextState*>(state)->mu);
        session = state->memory_provider;
        if (session == nullptr && !state->memory_quarantine.empty())
            session = state->memory_quarantine.front();
    }
    if (session == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->cleanup_status != SAO_SDK_OK)
        return session->cleanup_status;
    return session->accepting ? SAO_SDK_OK : SAO_SDK_ERR_BUSY;
}

sao_sdk_status_t memory_attachment_status(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (memory_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    std::shared_ptr<MemoryProviderSession> session;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        session = state->memory_provider;
    }
    if (session == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->cleanup_status != SAO_SDK_OK)
        return session->cleanup_status;
    if (!session->accepting)
        return SAO_SDK_ERR_BUSY;
    return session->attached ? SAO_SDK_OK : SAO_SDK_ERR_NOT_INITIALIZED;
}

sao_sdk_status_t memory_provider_cleanup(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    return configure_memory_provider(state, nullptr, ProviderConfigureOrigin::cleanup);
}

sao_sdk_status_t memory_attach(ContextState* state, const SaoSdkMemoryTargetIdentity* identity) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (identity == nullptr ||
        identity->struct_size < SAO_SDK_MEMORY_TARGET_IDENTITY_REQUIRED_SIZE) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    SaoSdkMemoryTargetIdentity normalized{};
    std::memcpy(&normalized, identity, std::min<size_t>(identity->struct_size, sizeof(normalized)));
    if (normalized.abi_version != 0 &&
        (normalized.abi_version >> 16) != SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MAJOR) {
        return SAO_SDK_ERR_ABI_MISMATCH;
    }
    const bool has_image_name =
        identity->struct_size >= offsetof(SaoSdkMemoryTargetIdentity, image_name_utf8) + 1;
    if (normalized.process_id == 0 && (!has_image_name || normalized.image_name_utf8[0] == '\0'))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (has_image_name && std::memchr(normalized.image_name_utf8, '\0',
                                      sizeof(normalized.image_name_utf8)) == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    normalized.struct_size = sizeof(normalized);
    normalized.abi_version = SAO_SDK_MEMORY_PROVIDER_ABI_VERSION;
    MemoryCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    const auto status = invoke_callback_barrier([&] {
        MemoryCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(
            lease->provider.attach(lease->provider.user_data, lease->session, &normalized));
    });
    if (status == SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(lease->mutex);
        lease->attached = true;
    }
    return status;
}

sao_sdk_status_t memory_detach(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    MemoryCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    if (!memory_session_attached(lease.operator->()))
        return SAO_SDK_OK;
    const auto status = invoke_callback_barrier([&] {
        MemoryCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(
            lease->provider.detach(lease->provider.user_data, lease->session));
    });
    if (status == SAO_SDK_OK) {
        std::lock_guard<std::mutex> lock(lease->mutex);
        lease->attached = false;
    }
    return status;
}

sao_sdk_status_t memory_read(ContextState* state, uint64_t address, void* out_buffer,
                             size_t buffer_size, size_t* out_bytes_read) {
    if (out_bytes_read != nullptr)
        *out_bytes_read = 0;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if ((buffer_size != 0 && out_buffer == nullptr) || out_bytes_read == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    MemoryCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    if (!memory_session_attached(lease.operator->()))
        return SAO_SDK_ERR_NOT_INITIALIZED;
    if (buffer_size != 0 && address == 0)
        return SAO_SDK_ERR_READ_FAULT;
    size_t bytes_read = 0;
    const auto status = invoke_callback_barrier([&] {
        MemoryCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(lease->provider.read(lease->provider.user_data,
                                                              lease->session, address, out_buffer,
                                                              buffer_size, &bytes_read));
    });
    if (bytes_read > buffer_size) {
        if (out_buffer != nullptr && buffer_size != 0)
            std::memset(out_buffer, 0, buffer_size);
        return SAO_SDK_ERR_INTERNAL;
    }
    *out_bytes_read = bytes_read;
    return status;
}

sao_sdk_status_t memory_enumerate_modules(ContextState* state, SaoSdkMemoryModule* out_modules,
                                          size_t capacity, size_t element_stride,
                                          size_t* out_count) {
    if (out_count != nullptr)
        *out_count = 0;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (out_count == nullptr || (capacity != 0 && out_modules == nullptr) ||
        element_stride != SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE ||
        capacity > SAO_SDK_MEMORY_MAX_MODULE_COUNT) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    MemoryCallLease lease(state);
    if (!lease)
        return lease.status();
    std::lock_guard<std::mutex> operation_lock(lease->operation_mutex);
    if (!memory_session_attached(lease.operator->()))
        return SAO_SDK_ERR_NOT_INITIALIZED;
    const auto status = invoke_callback_barrier([&] {
        MemoryCallbackScope callback_scope(state, lease.operator->());
        return normalize_provider_status(
            lease->provider.enumerate_modules(lease->provider.user_data, lease->session,
                                              out_modules, capacity, element_stride, out_count));
    });
    if (*out_count > SAO_SDK_MEMORY_MAX_MODULE_COUNT) {
        *out_count = 0;
        return SAO_SDK_ERR_INTERNAL;
    }
    if (status == SAO_SDK_OK && *out_count > capacity)
        return SAO_SDK_ERR_BUFFER_TOO_SMALL;
    if (status == SAO_SDK_OK) {
        for (size_t index = 0; index < *out_count; ++index) {
            const auto& module = out_modules[index];
            if (module.struct_size != SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE ||
                (module.abi_version >> 16) != SAO_SDK_MEMORY_PROVIDER_ABI_VERSION_MAJOR ||
                std::memchr(module.name_utf8, '\0', sizeof(module.name_utf8)) == nullptr) {
                *out_count = 0;
                return SAO_SDK_ERR_ABI_MISMATCH;
            }
        }
    }
    return status;
}

sao_sdk_status_t provider_tts_speak(ContextState* state, const char* text_utf8, float volume,
                                    float rate) {
    if (!std::isfinite(volume) || !std::isfinite(rate))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (text_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.tts_speak == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    return invoke_provider_callback(
        state, [&] { return provider.tts_speak(provider.user_data, text_utf8, volume, rate); });
}

sao_sdk_status_t provider_tts_stop(ContextState* state) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.tts_stop == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    return invoke_provider_callback(state, [&] { return provider.tts_stop(provider.user_data); });
}

sao_sdk_status_t provider_render_register(ContextState* state, int32_t hook_point,
                                          sao_sdk_render_hook_callback_t callback, void* user_data,
                                          sao_sdk_hook_token_t* out_token) {
    const SaoSdkRenderHookSpec spec{SAO_ENGINE_ALL_SURFACES, hook_point, 0.0F};
    return provider_render_register_ex(state, &spec, callback, user_data, out_token);
}

sao_sdk_status_t provider_render_register_ex(ContextState* state, const SaoSdkRenderHookSpec* spec,
                                             sao_sdk_render_hook_callback_t callback,
                                             void* user_data, sao_sdk_hook_token_t* out_token) {
    if (out_token != nullptr)
        *out_token = 0;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (spec == nullptr || spec->surface_id_utf8 == nullptr || spec->surface_id_utf8[0] == '\0' ||
        callback == nullptr || out_token == nullptr || spec->hook_point < 0 ||
        spec->hook_point > SAO_SDK_HOOK_AFTER_PRESENT || !std::isfinite(spec->priority)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if ((provider.register_render_hook == nullptr && provider.register_render_hook_ex == nullptr) ||
        provider.unregister_render_hook == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = allocate_capability_token(state);
    auto* bridge = new (std::nothrow) RenderCallbackBridge{state, callback, user_data};
    if (bridge == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    sao_sdk_status_t status = SAO_SDK_ERR_UNSUPPORTED;
    if (provider.register_render_hook_ex != nullptr) {
        status = invoke_provider_callback(state, [&] {
            return provider.register_render_hook_ex(provider.user_data, state->plugin_id.c_str(),
                                                    spec, render_callback_bridge, bridge,
                                                    &provider_token);
        });
    } else if (provider.register_render_hook != nullptr &&
               std::strcmp(spec->surface_id_utf8, SAO_ENGINE_ALL_SURFACES) == 0 &&
               spec->priority == 0.0F) {
        status = invoke_provider_callback(state, [&] {
            return provider.register_render_hook(provider.user_data, state->plugin_id.c_str(),
                                                 spec->hook_point, render_callback_bridge, bridge,
                                                 &provider_token);
        });
    }
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    CapabilityRegistration registration{CapabilityKind::render_hook, sdk_token, provider_token,
                                        bridge, destroy_render_bridge};
    const auto add_status = add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        return rollback_added_registration(state, provider, registration, add_status);
    }
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        if (g_fail_next_render_state_insertion.exchange(false))
            throw std::bad_alloc{};
        state->render_hooks.push_back(RenderHookEntry{sdk_token, spec->hook_point, callback,
                                                      user_data, spec->surface_id_utf8,
                                                      spec->priority});
    } catch (...) {
        return rollback_added_registration(state, provider, registration,
                                           SAO_SDK_ERR_NOT_INITIALIZED);
    }
    *out_token = sdk_token;
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_request_redraw(ContextState* state, const char* surface_id_utf8) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.request_redraw == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    return invoke_provider_callback(
        state, [&] { return provider.request_redraw(provider.user_data, surface_id_utf8); });
}

sao_sdk_status_t provider_render_unregister(ContextState* state, sao_sdk_hook_token_t token) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (token == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_callback_reentered(state) || plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::render_hook, token, &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    const auto status =
        normalize_unregister_status(unregister_provider_token(state, provider, registration));
    if (status != SAO_SDK_OK) {
        restore_registration(state, registration);
        return status;
    }
    finish_registration(state, registration);
    std::lock_guard<std::mutex> lock(state->mu);
    std::erase_if(state->render_hooks,
                  [token](const RenderHookEntry& entry) { return entry.token == token; });
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_hotkey_register(ContextState* state, const char* binding_id_utf8,
                                          uint32_t virtual_key, uint32_t modifiers,
                                          sao_sdk_hotkey_callback_t callback, void* user_data,
                                          sao_sdk_hotkey_id_t* out_id) {
    if (out_id != nullptr)
        *out_id = 0;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (binding_id_utf8 == nullptr || binding_id_utf8[0] == '\0' || callback == nullptr ||
        out_id == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.register_hotkey == nullptr || provider.unregister_hotkey == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = allocate_capability_token(state);
    auto* bridge = new (std::nothrow) HotkeyCallbackBridge{state, sdk_token, callback, user_data};
    if (bridge == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    const auto status = invoke_provider_callback(state, [&] {
        return provider.register_hotkey(provider.user_data, state->plugin_id.c_str(),
                                        binding_id_utf8, virtual_key, modifiers,
                                        hotkey_callback_bridge, bridge, &provider_token);
    });
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    CapabilityRegistration registration{CapabilityKind::hotkey, sdk_token, provider_token, bridge,
                                        destroy_hotkey_provider_bridge};
    const auto add_status = add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        return rollback_added_registration(state, provider, registration, add_status);
    }
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        if (g_fail_next_hotkey_state_insertion.exchange(false))
            throw std::bad_alloc{};
        HotkeyEntry entry{};
        entry.sdk_id = sdk_token;
        entry.plugin_cb = callback;
        entry.plugin_ud = user_data;
        entry.binding_id = binding_id_utf8;
        entry.bridge = bridge;
        state->hotkeys.push_back(std::move(entry));
    } catch (...) {
        return rollback_added_registration(state, provider, registration,
                                           SAO_SDK_ERR_NOT_INITIALIZED);
    }
    *out_id = sdk_token;
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_hotkey_unregister(ContextState* state, sao_sdk_hotkey_id_t id) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (id == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_callback_reentered(state) || plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::hotkey, id, &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    const auto status =
        normalize_unregister_status(unregister_provider_token(state, provider, registration));
    if (status != SAO_SDK_OK) {
        restore_registration(state, registration);
        return status;
    }
    finish_registration(state, registration);
    std::lock_guard<std::mutex> lock(state->mu);
    std::erase_if(state->hotkeys, [id](const HotkeyEntry& entry) { return entry.sdk_id == id; });
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_overlay_set(ContextState* state, const SaoSdkOverlaySpec* spec,
                                      sao_sdk_overlay_token_t* out_overlay) {
    if (out_overlay != nullptr)
        *out_overlay = 0;
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (spec == nullptr || spec->surface_id_utf8 == nullptr || spec->surface_id_utf8[0] == '\0' ||
        out_overlay == nullptr || (spec->spec_len != 0 && spec->spec_json_utf8 == nullptr)) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    if (provider_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.set_overlay == nullptr || provider.clear_overlay == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const std::string surface(spec->surface_id_utf8);
    const auto sdk_token = allocate_capability_token(state);
    if (sdk_token == 0)
        return SAO_SDK_ERR_INTERNAL;
    CapabilityRegistration previous{};
    bool inserted_surface = false;
    {
        std::lock_guard lock(state->mu);
        const auto found = state->overlays.find(surface);
        if (found != state->overlays.end()) {
            const auto old_token = reinterpret_cast<uint64_t>(found->second);
            const auto old_registration = std::find_if(
                state->capability_registrations.begin(), state->capability_registrations.end(),
                [old_token](const auto& item) {
                    return item.kind == CapabilityKind::overlay && item.sdk_token == old_token;
                });
            if (old_token == 0 || old_registration == state->capability_registrations.end() ||
                old_registration->unregistering)
                return SAO_SDK_ERR_BUSY;
            previous = *old_registration;
        }
        try {
            if (g_fail_next_overlay_state_insertion.exchange(false))
                throw std::bad_alloc{};
            inserted_surface = state->overlays.try_emplace(surface, nullptr).second;
            state->capability_registrations.push_back(
                {CapabilityKind::overlay, sdk_token, 0, nullptr, nullptr, true});
        } catch (...) {
            if (inserted_surface)
                state->overlays.erase(surface);
            return SAO_SDK_ERR_INTERNAL;
        }
        for (auto& item : state->capability_registrations) {
            if (item.kind == CapabilityKind::overlay && item.sdk_token == previous.sdk_token)
                item.unregistering = true;
        }
    }
    uint64_t provider_token = 0;
    auto status = invoke_provider_callback(state, [&] {
        return provider.set_overlay(provider.user_data, state->plugin_id.c_str(), spec,
                                    &provider_token);
    });
    if (provider_token == 0) {
        std::lock_guard lock(state->mu);
        std::erase_if(state->capability_registrations, [sdk_token](const auto& item) {
            return item.kind == CapabilityKind::overlay && item.sdk_token == sdk_token;
        });
        for (auto& item : state->capability_registrations) {
            if (item.kind == CapabilityKind::overlay && item.sdk_token == previous.sdk_token)
                item.unregistering = false;
        }
        if (inserted_surface)
            state->overlays.erase(surface);
        return status == SAO_SDK_OK ? SAO_SDK_ERR_HANDLE_INVALID : status;
    }
    bool committed = status == SAO_SDK_OK;
    if (!committed && provider.set_overlay == platform_overlay_set) {
        auto* native = static_cast<PlatformProviderState*>(provider.user_data);
        std::lock_guard lock(native->mutex);
        const auto found = native->overlays.find(provider_token);
        committed = found != native->overlays.end() && found->second->registry_owned;
    }
    sao_sdk_status_t cleanup_status = SAO_SDK_OK;
    if (committed && previous.sdk_token != 0 && previous.provider_token != provider_token)
        cleanup_status =
            normalize_unregister_status(unregister_provider_token(state, provider, previous));
    {
        std::lock_guard lock(state->mu);
        for (auto& item : state->capability_registrations) {
            if (item.kind != CapabilityKind::overlay)
                continue;
            if (item.sdk_token == sdk_token) {
                item.provider_token = provider_token;
                item.unregistering = false;
            } else if (item.sdk_token == previous.sdk_token) {
                item.unregistering = false;
            }
        }
        if (committed && previous.sdk_token != 0 && cleanup_status == SAO_SDK_OK) {
            std::erase_if(state->capability_registrations, [&previous](const auto& item) {
                return item.kind == CapabilityKind::overlay && item.sdk_token == previous.sdk_token;
            });
        }
        if (committed || inserted_surface)
            state->overlays.find(surface)->second = reinterpret_cast<sao_sdk_ui_panel_t>(sdk_token);
    }
    *out_overlay = sdk_token;
    if (committed)
        return SAO_SDK_OK;
    return status != SAO_SDK_OK ? status : cleanup_status;
}

sao_sdk_status_t provider_overlay_clear(ContextState* state, sao_sdk_overlay_token_t overlay) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (overlay == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::overlay, overlay, &registration, &provider)) {
        std::lock_guard lock(state->mu);
        const bool pending = std::any_of(
            state->capability_registrations.begin(), state->capability_registrations.end(),
            [overlay](const auto& item) {
                return item.kind == CapabilityKind::overlay && item.sdk_token == overlay;
            });
        return pending ? SAO_SDK_ERR_BUSY : SAO_SDK_ERR_NOT_FOUND;
    }
    const auto status =
        normalize_unregister_status(unregister_provider_token(state, provider, registration));
    if (status != SAO_SDK_OK) {
        restore_registration(state, registration);
        return status;
    }
    std::lock_guard<std::mutex> lock(state->mu);
    std::erase_if(state->capability_registrations, [overlay](const auto& item) {
        return item.kind == CapabilityKind::overlay && item.sdk_token == overlay;
    });
    std::erase_if(state->overlays, [overlay](const auto& item) {
        return reinterpret_cast<uint64_t>(item.second) == overlay;
    });
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_overlay_clear_surface(ContextState* state, const char* surface_id_utf8) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (surface_id_utf8 == nullptr || surface_id_utf8[0] == '\0')
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    uint64_t overlay = 0;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto found = state->overlays.find(surface_id_utf8);
        if (found == state->overlays.end())
            return SAO_SDK_ERR_NOT_FOUND;
        overlay = reinterpret_cast<uint64_t>(found->second);
    }
    if (overlay == 0)
        return SAO_SDK_ERR_BUSY;
    return provider_overlay_clear(state, overlay);
}

sao_sdk_status_t retain_gpu_provider(ContextState* state, SaoSdkProviderVTable* out_provider,
                                     std::string* out_plugin_id) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (out_provider == nullptr || out_plugin_id == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.retain == nullptr || provider.release == nullptr ||
        provider.gpu_hunt_open_session == nullptr || provider.gpu_hunt_close_session == nullptr ||
        provider.gpu_hunt_attach == nullptr || provider.gpu_hunt_detach == nullptr ||
        provider.gpu_hunt_enum_regions == nullptr || provider.gpu_hunt_read == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto status = invoke_provider_callback(state, [&] {
        provider.retain(provider.user_data);
        return SAO_SDK_OK;
    });
    if (status != SAO_SDK_OK)
        return status;
    *out_provider = provider;
    *out_plugin_id = state->plugin_id;
    return SAO_SDK_OK;
}

#if defined(SAO_SDK_TESTING)
extern "C" SAO_SDK_API void*
    SAO_SDK_CALL sao_sdk_test_platform_hotkey_snapshot_user_data(const SaoSdkContext* ctx,
                                                                 sao_sdk_hotkey_id_t hotkey) {
    ContextApiLease lease(ctx);
    if (!lease)
        return nullptr;
    auto* state = lease.state();
    uint64_t provider_token = 0;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto found = std::find_if(state->capability_registrations.begin(),
                                        state->capability_registrations.end(),
                                        [hotkey](const CapabilityRegistration& registration) {
                                            return registration.kind == CapabilityKind::hotkey &&
                                                   registration.sdk_token == hotkey;
                                        });
        if (found == state->capability_registrations.end() ||
            state->provider.user_data != &platform_provider_state()) {
            return nullptr;
        }
        provider_token = found->provider_token;
    }
    auto& platform_state = platform_provider_state();
    std::lock_guard<std::mutex> lock(platform_state.mutex);
    const auto found = platform_state.hotkeys.find(provider_token);
    if (found == platform_state.hotkeys.end())
        return nullptr;
    return new (std::nothrow) std::shared_ptr<PlatformHotkeyEntry>(found->second);
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_invoke_platform_hotkey_snapshot(void* snapshot_user_data) {
    SaoUiInputEvent event{};
    event.kind = SAO_UI_INPUT_KEY_DOWN;
    auto* snapshot = static_cast<std::shared_ptr<PlatformHotkeyEntry>*>(snapshot_user_data);
    platform_hotkey_callback(
        nullptr, &event, snapshot == nullptr || *snapshot == nullptr ? nullptr : snapshot->get());
}

extern "C" SAO_SDK_API void SAO_SDK_CALL
sao_sdk_test_release_platform_hotkey_snapshot(void* snapshot_user_data) {
    delete static_cast<std::shared_ptr<PlatformHotkeyEntry>*>(snapshot_user_data);
}
#endif

} // namespace sao_sdk_internal

#if defined(SAO_SDK_TESTING)
extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_timer_insertion(void) {
    sao_sdk_internal::test_fail_next_platform_timer_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_hotkey_insertion(void) {
    sao_sdk_internal::test_fail_next_platform_hotkey_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_dialog_insertion(void) {
    sao_sdk_internal::test_fail_next_platform_dialog_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_platform_overlay_insertion(void) {
    sao_sdk_internal::test_fail_next_platform_overlay_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_render_state_insertion(void) {
    sao_sdk_internal::test_fail_next_render_state_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_hotkey_state_insertion(void) {
    sao_sdk_internal::test_fail_next_hotkey_state_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_notify_state_insertion(void) {
    sao_sdk_internal::test_fail_next_notify_state_insertion();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_fail_next_overlay_state_insertion(void) {
    sao_sdk_internal::test_fail_next_overlay_state_insertion();
}
#endif

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_provider(SaoSdkContext* ctx, const SaoSdkProviderVTable* provider) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier(
        [&] { return sao_sdk_internal::bind_provider(lease.state(), provider); });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_platform_services(SaoSdkContext* ctx) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier(
        [&] { return sao_sdk_internal::bind_platform_services(lease.state()); });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_gpu_hunt_configure_provider(const SaoSdkProviderVTable* provider) {
    return sao_sdk_internal::configure_platform_gpu_hunt_provider(provider);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_configure_memory_provider(
    SaoSdkContext* ctx, const SaoSdkMemoryProviderVTable* provider) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier([&] {
        return sao_sdk_internal::configure_memory_provider(
            lease.state(), provider, sao_sdk_internal::ProviderConfigureOrigin::public_api);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_memory_provider_status(const SaoSdkContext* ctx) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier(
        [&] { return sao_sdk_internal::memory_provider_status(lease.state()); });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_provider_status(const SaoSdkContext* ctx) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier(
        [&] { return sao_sdk_internal::provider_status(lease.state()); });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_timer_register(
    const SaoSdkContext* ctx, uint32_t interval_ms, sao_sdk_timer_callback_t callback,
    void* user_data, sao_sdk_timer_token_t* out_timer) {
    sao_sdk_internal::ContextApiLease context_lease(ctx);
    if (!context_lease)
        return context_lease.status();
    if (out_timer != nullptr)
        *out_timer = 0;
    if (callback == nullptr || out_timer == nullptr || interval_ms == 0) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = context_lease.state();
    sao_sdk_internal::ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.register_timer == nullptr || provider.unregister_timer == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = sao_sdk_internal::allocate_capability_token(state);
    auto* bridge = new (std::nothrow)
        sao_sdk_internal::TimerCallbackBridge{state, sdk_token, callback, user_data};
    if (bridge == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    const auto status = sao_sdk_internal::invoke_provider_callback(state, [&] {
        return provider.register_timer(provider.user_data, interval_ms,
                                       sao_sdk_internal::timer_callback_bridge, bridge,
                                       &provider_token);
    });
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    sao_sdk_internal::CapabilityRegistration registration{sao_sdk_internal::CapabilityKind::timer,
                                                          sdk_token, provider_token, bridge,
                                                          sao_sdk_internal::destroy_timer_bridge};
    const auto add_status = sao_sdk_internal::add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        return sao_sdk_internal::rollback_added_registration(state, provider, registration,
                                                             add_status);
    }
    *out_timer = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_timer_unregister(const SaoSdkContext* ctx, sao_sdk_timer_token_t timer) {
    sao_sdk_internal::ContextApiLease context_lease(ctx);
    if (!context_lease)
        return context_lease.status();
    if (timer == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = context_lease.state();
    if (sao_sdk_internal::provider_callback_reentered(state) ||
        sao_sdk_internal::plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    sao_sdk_internal::ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    sao_sdk_internal::CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!sao_sdk_internal::take_registration(state, sao_sdk_internal::CapabilityKind::timer, timer,
                                             &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    const auto status = sao_sdk_internal::normalize_unregister_status(
        sao_sdk_internal::unregister_provider_token(state, provider, registration));
    if (status != SAO_SDK_OK) {
        sao_sdk_internal::restore_registration(state, registration);
        return status;
    }
    sao_sdk_internal::finish_registration(state, registration);
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_dialog_show(
    const SaoSdkContext* ctx, const SaoSdkDialogSpec* spec, sao_sdk_dialog_callback_t callback,
    void* user_data, sao_sdk_dialog_token_t* out_dialog) {
    sao_sdk_internal::ContextApiLease context_lease(ctx);
    if (!context_lease)
        return context_lease.status();
    if (out_dialog != nullptr)
        *out_dialog = 0;
    if (spec == nullptr || out_dialog == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = context_lease.state();
    sao_sdk_internal::ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.show_dialog == nullptr || provider.dismiss_dialog == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = sao_sdk_internal::allocate_capability_token(state);
    auto* bridge = new (std::nothrow)
        sao_sdk_internal::DialogCallbackBridge{state, sdk_token, callback, user_data};
    if (bridge == nullptr)
        return SAO_SDK_ERR_NOT_INITIALIZED;
    uint64_t provider_token = 0;
    const auto status = sao_sdk_internal::invoke_provider_callback(state, [&] {
        return provider.show_dialog(provider.user_data, state->plugin_id.c_str(), spec,
                                    sao_sdk_internal::dialog_callback_bridge, bridge,
                                    &provider_token);
    });
    if (status != SAO_SDK_OK) {
        delete bridge;
        return status;
    }
    if (provider_token == 0) {
        delete bridge;
        return SAO_SDK_ERR_HANDLE_INVALID;
    }
    sao_sdk_internal::CapabilityRegistration registration{sao_sdk_internal::CapabilityKind::dialog,
                                                          sdk_token, provider_token, bridge,
                                                          sao_sdk_internal::destroy_dialog_bridge};
    const auto add_status = sao_sdk_internal::add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        return sao_sdk_internal::rollback_added_registration(state, provider, registration,
                                                             add_status);
    }
    *out_dialog = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_dialog_dismiss(const SaoSdkContext* ctx, sao_sdk_dialog_token_t dialog) {
    sao_sdk_internal::ContextApiLease context_lease(ctx);
    if (!context_lease)
        return context_lease.status();
    if (dialog == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = context_lease.state();
    if (sao_sdk_internal::provider_callback_reentered(state) ||
        sao_sdk_internal::plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    sao_sdk_internal::ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    sao_sdk_internal::CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!sao_sdk_internal::take_registration(state, sao_sdk_internal::CapabilityKind::dialog,
                                             dialog, &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    const auto status = sao_sdk_internal::normalize_unregister_status(
        sao_sdk_internal::unregister_provider_token(state, provider, registration));
    if (status != SAO_SDK_OK) {
        sao_sdk_internal::restore_registration(state, registration);
        return status;
    }
    sao_sdk_internal::finish_registration(state, registration);
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_notify_show(
    const SaoSdkContext* ctx, const SaoSdkNotifySpec* spec, sao_sdk_notify_token_t* out_notify) {
    sao_sdk_internal::ContextApiLease context_lease(ctx);
    if (!context_lease)
        return context_lease.status();
    if (out_notify != nullptr)
        *out_notify = 0;
    if (spec == nullptr || spec->text_utf8 == nullptr || out_notify == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = context_lease.state();
    sao_sdk_internal::ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.show_notify == nullptr || provider.dismiss_notify == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = sao_sdk_internal::allocate_capability_token(state);
    uint64_t provider_token = 0;
    const auto status = sao_sdk_internal::invoke_provider_callback(state, [&] {
        return provider.show_notify(provider.user_data, state->plugin_id.c_str(), spec,
                                    &provider_token);
    });
    if (status != SAO_SDK_OK)
        return status;
    if (provider_token == 0)
        return SAO_SDK_ERR_HANDLE_INVALID;
    sao_sdk_internal::CapabilityRegistration registration{
        sao_sdk_internal::CapabilityKind::notify, sdk_token, provider_token, nullptr, nullptr};
    const auto add_status = sao_sdk_internal::add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        return sao_sdk_internal::rollback_added_registration(state, provider, registration,
                                                             add_status);
    }
    try {
        std::lock_guard<std::mutex> lock(state->mu);
        if (sao_sdk_internal::g_fail_next_notify_state_insertion.exchange(false))
            throw std::bad_alloc{};
        state->banner_ids.push_back(sdk_token);
    } catch (...) {
        return sao_sdk_internal::rollback_added_registration(state, provider, registration,
                                                             SAO_SDK_ERR_NOT_INITIALIZED);
    }
    *out_notify = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_notify_dismiss(const SaoSdkContext* ctx, sao_sdk_notify_token_t notify) {
    sao_sdk_internal::ContextApiLease context_lease(ctx);
    if (!context_lease)
        return context_lease.status();
    if (notify == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = context_lease.state();
    if (sao_sdk_internal::provider_callback_reentered(state) ||
        sao_sdk_internal::plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    sao_sdk_internal::ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    sao_sdk_internal::CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!sao_sdk_internal::take_registration(state, sao_sdk_internal::CapabilityKind::notify,
                                             notify, &registration, &provider)) {
        return SAO_SDK_ERR_NOT_FOUND;
    }
    const auto status = sao_sdk_internal::normalize_unregister_status(
        sao_sdk_internal::unregister_provider_token(state, provider, registration));
    if (status != SAO_SDK_OK) {
        sao_sdk_internal::restore_registration(state, registration);
        return status;
    }
    sao_sdk_internal::finish_registration(state, registration);
    std::lock_guard<std::mutex> lock(state->mu);
    std::erase(state->banner_ids, notify);
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_overlay_set(
    const SaoSdkContext* ctx, const SaoSdkOverlaySpec* spec, sao_sdk_overlay_token_t* out_overlay) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier(
        [&] { return sao_sdk_internal::provider_overlay_set(lease.state(), spec, out_overlay); });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_overlay_clear(const SaoSdkContext* ctx, sao_sdk_overlay_token_t overlay) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier(
        [&] { return sao_sdk_internal::provider_overlay_clear(lease.state(), overlay); });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_overlay_clear_surface(const SaoSdkContext* ctx, const char* surface_id_utf8) {
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    return sao_sdk_internal::invoke_callback_barrier([&] {
        return sao_sdk_internal::provider_overlay_clear_surface(lease.state(), surface_id_utf8);
    });
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_platform_render_dispatch(
    const char* surface_id_utf8, int32_t hook_point, uint64_t monotonic_time_ns,
    int32_t viewport_x_px, int32_t viewport_y_px, int32_t viewport_width_px,
    int32_t viewport_height_px, uint32_t dispatch_flags) {
    constexpr uint32_t kAllowedSources = SAO_SDK_RENDER_DISPATCH_LOGICAL_TICK |
                                         SAO_SDK_RENDER_DISPATCH_COMPOSITOR_PRESENT |
                                         SAO_SDK_RENDER_DISPATCH_GPU_PRESENT;
    if ((dispatch_flags & ~kAllowedSources) != 0) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    auto& runtime = sao_sdk_internal::SharedRuntime::instance();
    const sao_sdk_status_t runtime_status = runtime.ensure_started();
    if (runtime_status != SAO_SDK_OK)
        return runtime_status;
    return sao_sdk_internal::normalize_provider_status(static_cast<sao_sdk_status_t>(
        sao_engine_render_clock_dispatch(runtime.render_registry, surface_id_utf8, hook_point,
                                         monotonic_time_ns, viewport_x_px, viewport_y_px,
                                         viewport_width_px, viewport_height_px, dispatch_flags)));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_render_gpu_provider_status(void) {
    auto& runtime = sao_sdk_internal::SharedRuntime::instance();
    const sao_sdk_status_t runtime_status = runtime.ensure_started();
    if (runtime_status != SAO_SDK_OK)
        return runtime_status;
    const sao_status_t status = sao_engine_render_hook_provider_status(runtime.render_registry);
    return sao_sdk_internal::normalize_provider_status(static_cast<sao_sdk_status_t>(status));
}
