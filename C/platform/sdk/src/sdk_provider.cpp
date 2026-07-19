#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "sao/core/thread.h"
#include "sao/ui/alerts.h"
#include "sao/ui/dialog.h"

namespace sao_sdk_internal {

thread_local ContextState* g_memory_callback_owner = nullptr;
thread_local MemoryProviderSession* g_memory_callback_session = nullptr;

bool memory_callback_reentered(ContextState* state) noexcept {
    return state != nullptr && g_memory_callback_owner == state;
}

namespace {

constexpr uint32_t kProviderMinimumSize =
    static_cast<uint32_t>(offsetof(SaoSdkProviderVTable, retain));
constexpr uint32_t kGpuHuntProviderMinimumSize =
    static_cast<uint32_t>(offsetof(SaoSdkProviderVTable, gpu_hunt_read) +
                          sizeof(static_cast<SaoSdkProviderVTable*>(nullptr)->gpu_hunt_read));
constexpr uint32_t kMemoryProviderMinimumSize = SAO_SDK_MEMORY_PROVIDER_REQUIRED_SIZE;

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
    std::lock_guard<std::mutex> lock(state->mu);
    return state->next_capability_token++;
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
        std::unique_lock<std::mutex> lock(state->callback_mutex);
        state->callback_idle.wait(lock, [state] { return state->active_plugin_callbacks == 0; });
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
};

struct PlatformDialogEntry {
    sao_ui_dialog_handle_t dialog = nullptr;
    sao_sdk_dialog_callback_t callback = nullptr;
    void* user_data = nullptr;
};

struct PlatformOverlayEntry {
    std::string plugin_id;
    std::string surface_id;
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
    std::unordered_map<uint64_t, PlatformOverlayEntry> overlays;
    std::shared_ptr<PlatformGpuHuntProvider> gpu_hunt_provider;
    std::atomic<uint64_t> next_token{1};
};

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
    if (value == nullptr)
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (length <= 0)
        return {};
    std::vector<uint16_t> result(static_cast<size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                            reinterpret_cast<wchar_t*>(result.data()), length) <= 0) {
        return {};
    }
    return result;
}

sao_sdk_status_t SAO_SDK_CALL platform_tts_speak(void*, const char* text_utf8, float volume,
                                                 float rate) {
    const auto text = utf8_to_utf16(text_utf8);
    if (text.empty())
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const int32_t native_rate = static_cast<int32_t>(std::clamp(rate, -10.0f, 10.0f));
    const int32_t native_volume = static_cast<int32_t>(std::clamp(volume, 0.0f, 1.0f) * 100.0f);
    return static_cast<sao_sdk_status_t>(
        sao_ui_alerts_speak(text.data(), nullptr, native_rate, native_volume));
}

sao_sdk_status_t SAO_SDK_CALL platform_tts_stop(void*) {
    // SAPI's SPF_PURGEBEFORESPEAK flag is exercised by alerts_speak.
    // A zero-volume word-joiner replaces the queue without producing audio.
    constexpr uint16_t kSilentPurge[] = {0x2060u, 0u};
    return static_cast<sao_sdk_status_t>(sao_ui_alerts_speak(kSilentPurge, nullptr, 0, 0));
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
    return static_cast<sao_status_t>(bridge->callback(hook_point, &sdk_payload, bridge->user_data));
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
        entry->callback(0, entry->user_data);
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
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->timers.emplace(token, entry);
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
    if (entry != nullptr && entry->callback != nullptr) {
        entry->callback(0, entry->user_data);
    }
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
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->hotkeys.emplace(token, entry);
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
    std::lock_guard<std::mutex> lock(state->mutex);
    state->hotkeys.erase(provider_token);
    return SAO_SDK_OK;
}

void SAO_UI_CALL platform_dialog_callback(SaoUiDialogButton pressed, const char* input_text_utf8,
                                          size_t input_text_len, void* user_data) {
    auto* entry = static_cast<PlatformDialogEntry*>(user_data);
    if (entry != nullptr && entry->callback != nullptr) {
        entry->callback(0, static_cast<int32_t>(pressed), input_text_utf8, input_text_len,
                        entry->user_data);
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
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dialogs.emplace(token, entry);
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

sao_sdk_status_t SAO_SDK_CALL platform_overlay_set(void* user_data, const char* plugin_id_utf8,
                                                   const SaoSdkOverlaySpec* spec,
                                                   uint64_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    if (plugin_id_utf8 == nullptr || spec == nullptr || spec->surface_id_utf8 == nullptr ||
        (spec->spec_len != 0 && spec->spec_json_utf8 == nullptr) || out_provider_token == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto& runtime = SharedRuntime::instance();
    if (runtime.render_registry == nullptr)
        return SAO_SDK_ERR_UNSUPPORTED;
    const sao_status_t status = sao_engine_render_hook_set_overlay(
        runtime.render_registry, plugin_id_utf8, spec->surface_id_utf8, spec->spec_json_utf8,
        spec->spec_len);
    if (status != SAO_STATUS_OK)
        return static_cast<sao_sdk_status_t>(status);
    auto* state = static_cast<PlatformProviderState*>(user_data);
    const uint64_t token = state->next_token.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->overlays.emplace(token, PlatformOverlayEntry{plugin_id_utf8, spec->surface_id_utf8});
    }
    *out_provider_token = token;
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL platform_overlay_clear(void* user_data, uint64_t provider_token) {
    auto* state = static_cast<PlatformProviderState*>(user_data);
    PlatformOverlayEntry entry;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->overlays.find(provider_token);
        if (found == state->overlays.end())
            return SAO_SDK_ERR_NOT_FOUND;
        entry = found->second;
    }
    const auto status = static_cast<sao_sdk_status_t>(
        sao_engine_render_hook_clear_overlay(SharedRuntime::instance().render_registry,
                                             entry.plugin_id.c_str(), entry.surface_id.c_str()));
    if (status != SAO_SDK_OK)
        return status;
    std::lock_guard<std::mutex> lock(state->mutex);
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

sao_sdk_status_t bind_provider(ContextState* state, const SaoSdkProviderVTable* provider) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
    SharedRuntime::instance().ensure_started();
    return bind_provider(state, platform_provider());
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
                                           const SaoSdkMemoryProviderVTable* provider) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (state->destroying.load(std::memory_order_acquire) &&
        !context_destroy_on_current_thread(state))
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
    return configure_memory_provider(state, nullptr);
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
        (void)invoke_provider_callback(state, [&] {
            return provider.unregister_render_hook(provider.user_data, provider_token);
        });
        delete bridge;
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->render_hooks.push_back(RenderHookEntry{sdk_token, spec->hook_point, callback,
                                                      user_data, spec->surface_id_utf8,
                                                      spec->priority});
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
        (void)invoke_provider_callback(
            state, [&] { return provider.unregister_hotkey(provider.user_data, provider_token); });
        delete bridge;
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        HotkeyEntry entry{};
        entry.sdk_id = sdk_token;
        entry.plugin_cb = callback;
        entry.plugin_ud = user_data;
        entry.binding_id = binding_id_utf8;
        entry.bridge = bridge;
        state->hotkeys.push_back(std::move(entry));
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
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    const auto& provider = lease.provider();
    if (provider.set_overlay == nullptr || provider.clear_overlay == nullptr) {
        return SAO_SDK_ERR_UNSUPPORTED;
    }
    const auto sdk_token = allocate_capability_token(state);
    uint64_t provider_token = 0;
    const auto status = invoke_provider_callback(state, [&] {
        return provider.set_overlay(provider.user_data, state->plugin_id.c_str(), spec,
                                    &provider_token);
    });
    if (status != SAO_SDK_OK)
        return status;
    if (provider_token == 0)
        return SAO_SDK_ERR_HANDLE_INVALID;
    CapabilityRegistration registration{CapabilityKind::overlay, sdk_token, provider_token, nullptr,
                                        nullptr};
    const auto add_status = add_registration(state, registration);
    if (add_status != SAO_SDK_OK) {
        (void)invoke_provider_callback(
            state, [&] { return provider.clear_overlay(provider.user_data, provider_token); });
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->overlays[spec->surface_id_utf8] = reinterpret_cast<sao_sdk_ui_panel_t>(sdk_token);
    }
    *out_overlay = sdk_token;
    return SAO_SDK_OK;
}

sao_sdk_status_t provider_overlay_clear(ContextState* state, sao_sdk_overlay_token_t overlay) {
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (overlay == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    if (provider_callback_reentered(state) || plugin_callback_reentered(state))
        return SAO_SDK_ERR_BUSY;
    ProviderCallLease lease(state);
    if (!lease)
        return lease.status();
    CapabilityRegistration registration;
    SaoSdkProviderVTable provider;
    if (!take_registration(state, CapabilityKind::overlay, overlay, &registration, &provider)) {
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
    std::erase_if(state->overlays, [overlay](const auto& item) {
        return reinterpret_cast<uint64_t>(item.second) == overlay;
    });
    return SAO_SDK_OK;
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

} // namespace sao_sdk_internal

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_provider(SaoSdkContext* ctx, const SaoSdkProviderVTable* provider) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::bind_provider(sao_sdk_internal::cast_ctx(ctx->ctx_impl), provider);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_bind_platform_services(SaoSdkContext* ctx) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    const auto bind_status = sao_sdk_internal::bind_platform_provider(state);
    if (bind_status != SAO_SDK_OK)
        return bind_status;
    return sao_sdk_internal::bind_process_providers(state);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_gpu_hunt_configure_provider(const SaoSdkProviderVTable* provider) {
    return sao_sdk_internal::configure_platform_gpu_hunt_provider(provider);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_configure_memory_provider(
    SaoSdkContext* ctx, const SaoSdkMemoryProviderVTable* provider) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::configure_memory_provider(sao_sdk_internal::cast_ctx(ctx->ctx_impl),
                                                       provider);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_memory_provider_status(const SaoSdkContext* ctx) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::memory_provider_status(sao_sdk_internal::cast_ctx(ctx->ctx_impl));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_provider_status(const SaoSdkContext* ctx) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_status(sao_sdk_internal::cast_ctx(ctx->ctx_impl));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_timer_register(
    const SaoSdkContext* ctx, uint32_t interval_ms, sao_sdk_timer_callback_t callback,
    void* user_data, sao_sdk_timer_token_t* out_timer) {
    if (out_timer != nullptr)
        *out_timer = 0;
    if (ctx == nullptr || callback == nullptr || out_timer == nullptr || interval_ms == 0) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
        (void)sao_sdk_internal::invoke_provider_callback(
            state, [&] { return provider.unregister_timer(provider.user_data, provider_token); });
        delete bridge;
        return add_status;
    }
    *out_timer = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_timer_unregister(const SaoSdkContext* ctx, sao_sdk_timer_token_t timer) {
    if (ctx == nullptr || timer == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
    if (out_dialog != nullptr)
        *out_dialog = 0;
    if (ctx == nullptr || spec == nullptr || out_dialog == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
        (void)sao_sdk_internal::invoke_provider_callback(
            state, [&] { return provider.dismiss_dialog(provider.user_data, provider_token); });
        delete bridge;
        return add_status;
    }
    *out_dialog = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_dialog_dismiss(const SaoSdkContext* ctx, sao_sdk_dialog_token_t dialog) {
    if (ctx == nullptr || dialog == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
    if (out_notify != nullptr)
        *out_notify = 0;
    if (ctx == nullptr || spec == nullptr || spec->text_utf8 == nullptr || out_notify == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
        (void)sao_sdk_internal::invoke_provider_callback(
            state, [&] { return provider.dismiss_notify(provider.user_data, provider_token); });
        return add_status;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->banner_ids.push_back(sdk_token);
    }
    *out_notify = sdk_token;
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_notify_dismiss(const SaoSdkContext* ctx, sao_sdk_notify_token_t notify) {
    if (ctx == nullptr || notify == 0)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    auto* state = sao_sdk_internal::cast_ctx(ctx->ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
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
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_overlay_set(sao_sdk_internal::cast_ctx(ctx->ctx_impl), spec,
                                                  out_overlay);
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_overlay_clear(const SaoSdkContext* ctx, sao_sdk_overlay_token_t overlay) {
    if (ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return sao_sdk_internal::provider_overlay_clear(sao_sdk_internal::cast_ctx(ctx->ctx_impl),
                                                    overlay);
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
    runtime.ensure_started();
    return sao_sdk_internal::normalize_provider_status(static_cast<sao_sdk_status_t>(
        sao_engine_render_clock_dispatch(runtime.render_registry, surface_id_utf8, hook_point,
                                         monotonic_time_ns, viewport_x_px, viewport_y_px,
                                         viewport_width_px, viewport_height_px, dispatch_flags)));
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_render_gpu_provider_status(void) {
    auto& runtime = sao_sdk_internal::SharedRuntime::instance();
    runtime.ensure_started();
    const sao_status_t status = sao_engine_render_hook_provider_status(runtime.render_registry);
    return sao_sdk_internal::normalize_provider_status(static_cast<sao_sdk_status_t>(status));
}
