#include "sao/launcher/init_pipeline.h"
#include "sao/launcher/provider_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_deps.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#include "sao/plugins/loader/plugin_scanner.h"
#include "sao_plugins/sao_status.h"

#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
#include "sao/plugins/emma_host/emma_loader_adapter.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
#include "sao/plugins/angel_host/as_call.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
#include "sao/plugins/lua_host/lua_host.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
#include "sao/plugins/csharp_host/cs_loader_adapter.h"
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
#ifdef SAO_STATUS_OK
#undef SAO_STATUS_OK
#endif
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct sao_plugins_registry_body {
    sao::plugins::loader::registry_handle_t registry = nullptr;
    std::vector<sao::plugins::loader::plugin_handle_t> handles;
    std::vector<bool> autostart;
    std::vector<sao::plugins::loader::plugin_manifest> manifests;
    int32_t python_runtime_status = SAO_PLUGINS_PYTHON_RUNTIME_HOST_UNAVAILABLE;
    int32_t python_launch_strategy = SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED;
    int32_t operational_status = SAO_PLUGINS_OPERATIONAL_READY;
    sao_status_t last_operation_status = SAO_STATUS_OK;
    sao_status_t last_rollback_status = SAO_STATUS_OK;
    bool rollback_attempted = false;
    bool rollback_succeeded = false;
    uint32_t deferred_count = 0;
    bool platform_provider_owned = false;
    bool deps_provider_owned = false;
    std::mutex operation_mutex;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    sao::plugins::emma_host::emma_loader_adapter_owner_t emma_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    sao::plugins::angel_host::as_loader_adapter_owner_t angel_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    sao::plugins::lua_host::lua_loader_adapter_owner_t lua_owner{};
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    sao::plugins::csharp_host::cs_loader_adapter_owner_t csharp_owner{};
#endif
};

struct sao_plugins_registry {
    std::mutex mutex;
    std::condition_variable idle;
    std::shared_ptr<sao_plugins_registry_body> body;
    uint32_t active_calls = 0;
    bool retiring = false;
    bool retired = false;
};

namespace {

namespace loader = sao::plugins::loader;

thread_local sao_plugins_registry* g_active_operation_registry = nullptr;

std::filesystem::path pathFromUtf8(std::string_view value) {
    const auto* first = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(first, first + value.size());
}

struct ProviderOwnerState {
    std::atomic_uint32_t leases{0};
};

ProviderOwnerState g_platform_provider_owner;
ProviderOwnerState g_deps_provider_owner;
std::atomic_uint64_t g_platform_session_sequence{0};

void SAO_PLUGINS_CALL retainProviderOwner(void* user_data) {
    if (user_data != nullptr) {
        static_cast<ProviderOwnerState*>(user_data)->leases.fetch_add(1, std::memory_order_relaxed);
    }
}

void SAO_PLUGINS_CALL releaseProviderOwner(void* user_data) {
    if (user_data != nullptr) {
        auto& leases = static_cast<ProviderOwnerState*>(user_data)->leases;
        uint32_t current = leases.load(std::memory_order_relaxed);
        while (current != 0 &&
               !leases.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
        }
    }
}

struct LauncherPlatformSession;

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
struct LauncherPlatformCompositorLayer {
    LauncherPlatformSession* session = nullptr;
    uint64_t provider_token = 0;
    sao_ui_compositor_handle_t compositor = nullptr;
    sao_ui_layer_handle_t layer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::mutex callback_mutex;
    std::condition_variable callback_idle;
    uint64_t callback_generation = 1;
    std::unordered_map<uint64_t, size_t> callbacks_by_generation;
    loader::compositor_cursor_pos_fn cursor_pos = nullptr;
    loader::compositor_mouse_button_fn mouse_button = nullptr;
    loader::compositor_cursor_leave_fn cursor_leave = nullptr;
    loader::compositor_scroll_fn scroll = nullptr;
    void* callback_user_data = nullptr;
};

struct LauncherPlatformTimer {
    LauncherPlatformSession* session = nullptr;
    loader::timer_callback_fn callback = nullptr;
    void* callback_user_data = nullptr;
    uint64_t provider_token = 0;
    sao_sdk_timer_token_t sdk_token = 0;
    bool one_shot = false;
    bool registering = true;
    bool published = false;
    bool pending_fired = false;
    bool fired = false;
    bool callback_active = false;
    bool cleanup_pending = false;
    bool unregistering = false;
};
#endif

struct LauncherPlatformSession {
    std::mutex mutex;
    std::condition_variable idle;
    std::string plugin_id;
    uint64_t session_id = 0;
    bool accepting_callbacks = true;
    size_t active_callbacks = 0;
    uint64_t next_token = 1;
    void* callback_gate_user_data = nullptr;
    bool(SAO_PLUGINS_CALL* enter_callback)(void*) = nullptr;
    void(SAO_PLUGINS_CALL* leave_callback)(void*) = nullptr;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    SaoSdkContext sdk_context{};
    bool sdk_ready = false;
    std::unordered_map<uint64_t, std::shared_ptr<LauncherPlatformTimer>> timers;
    std::unordered_map<uint64_t, sao_sdk_notify_token_t> notifications;
    std::unordered_map<uint64_t, std::shared_ptr<LauncherPlatformCompositorLayer>>
        compositor_layers;
    std::thread timer_cleanup_worker;
    bool timer_worker_stop = false;
    bool timer_worker_wake = false;
#endif
};

thread_local LauncherPlatformSession* g_platform_callback_session = nullptr;

class LoaderCallbackLease {
  public:
    explicit LoaderCallbackLease(LauncherPlatformSession* session) noexcept : session_(session) {
        if (session_ == nullptr || session_->enter_callback == nullptr ||
            session_->leave_callback == nullptr) {
            allowed_ = true;
            return;
        }
        entered_ = session_->enter_callback(session_->callback_gate_user_data);
        allowed_ = entered_;
    }

    ~LoaderCallbackLease() {
        if (entered_)
            session_->leave_callback(session_->callback_gate_user_data);
    }

    explicit operator bool() const noexcept {
        return allowed_;
    }

  private:
    LauncherPlatformSession* session_ = nullptr;
    bool entered_ = false;
    bool allowed_ = false;
};

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
struct ActiveLauncherCompositorCallback {
    LauncherPlatformCompositorLayer* layer = nullptr;
    uint64_t generation = 0;
    ActiveLauncherCompositorCallback* previous = nullptr;
};

thread_local ActiveLauncherCompositorCallback* g_active_compositor_callback = nullptr;

size_t compositorCallbackActiveCount(LauncherPlatformCompositorLayer* layer,
                                     uint64_t generation) noexcept {
    size_t count = 0;
    for (const auto* active = g_active_compositor_callback; active != nullptr;
         active = active->previous) {
        if (active->layer == layer && active->generation == generation)
            ++count;
    }
    return count;
}
#endif

uint64_t nextPlatformTokenLocked(LauncherPlatformSession& session) noexcept {
    for (;;) {
        const uint64_t candidate = session.next_token++;
        if (session.next_token == 0)
            session.next_token = 1;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
        if (candidate != 0 && !session.timers.contains(candidate) &&
            !session.notifications.contains(candidate) &&
            !session.compositor_layers.contains(candidate)) {
            return candidate;
        }
#else
        if (candidate != 0)
            return candidate;
#endif
    }
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
std::atomic_size_t g_platform_timer_count{0};
std::atomic_size_t g_platform_timer_worker_count{0};
std::atomic_uint64_t g_platform_timer_unregister_attempt_count{0};
std::atomic_bool g_test_fire_timer_during_register{false};
std::atomic<sao_sdk_status_t> g_test_fail_next_timer_unregister{SAO_SDK_OK};

int32_t mapSdkStatus(sao_sdk_status_t status) noexcept {
    switch (status) {
    case SAO_SDK_OK:
        return SAO_OK;
    case SAO_SDK_ERR_INVALID_ARGUMENT:
        return SAO_ERR_INVALID_ARGUMENT;
    case SAO_SDK_ERR_NOT_INITIALIZED:
        return SAO_ERR_NOT_INITIALIZED;
    case SAO_SDK_ERR_HANDLE_INVALID:
    case SAO_SDK_ERR_NOT_FOUND:
        return SAO_ERR_HANDLE_INVALID;
    case SAO_SDK_ERR_BUFFER_TOO_SMALL:
        return SAO_ERR_BUFFER_TOO_SMALL;
    case SAO_SDK_ERR_NOT_IMPLEMENTED:
        return SAO_ERR_NOT_IMPLEMENTED;
    case SAO_SDK_ERR_UNSUPPORTED:
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    case SAO_SDK_ERR_BUSY:
        return loader::SAO_PLUGINS_ERR_BUSY;
    case SAO_SDK_ERR_ACCESS_DENIED:
        return loader::SAO_PLUGINS_ERR_NOT_OWNER;
    case SAO_SDK_ERR_ALREADY_EXISTS:
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    case SAO_SDK_ERR_INTERNAL:
    default:
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t mapUiStatus(sao_status_t status) noexcept {
    switch (status) {
    case SAO_STATUS_OK:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    case SAO_STATUS_ERR_NOT_INITIALIZED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_INITIALIZED;
    case SAO_STATUS_ERR_HANDLE_INVALID:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUFFER_TOO_SMALL;
    case SAO_STATUS_ERR_NOT_IMPLEMENTED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_IMPLEMENTED;
    case SAO_STATUS_ERR_CANCELLED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUSY;
    case SAO_STATUS_ERR_ABI_MISMATCH:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ABI_MISMATCH;
    case SAO_STATUS_ERR_CAPABILITY_MISSING:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNSUPPORTED;
    case SAO_STATUS_ERR_OS_CALL_FAILED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_OS_CALL_FAILED;
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ACCESS_DENIED;
    case SAO_STATUS_ERR_NOT_FOUND:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_FOUND;
    case SAO_STATUS_ERR_ALREADY_EXISTS:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ALREADY_EXISTS;
    case SAO_STATUS_ERR_DEVICE_LOST:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_DEVICE_LOST;
    case SAO_STATUS_ERR_SURFACE_INVALID:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_SURFACE_INVALID;
    case SAO_STATUS_ERR_UNKNOWN:
    case SAO_STATUS_ERR_TIMEOUT:
    default:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

int32_t mapUiLoaderStatus(sao_status_t status) noexcept {
    switch (status) {
    case SAO_STATUS_OK:
        return SAO_OK;
    case SAO_STATUS_ERR_INVALID_ARGUMENT:
        return SAO_ERR_INVALID_ARGUMENT;
    case SAO_STATUS_ERR_NOT_INITIALIZED:
        return SAO_ERR_NOT_INITIALIZED;
    case SAO_STATUS_ERR_HANDLE_INVALID:
    case SAO_STATUS_ERR_NOT_FOUND:
    case SAO_STATUS_ERR_SURFACE_INVALID:
        return SAO_ERR_HANDLE_INVALID;
    case SAO_STATUS_ERR_BUFFER_TOO_SMALL:
        return SAO_ERR_BUFFER_TOO_SMALL;
    case SAO_STATUS_ERR_NOT_IMPLEMENTED:
    case SAO_STATUS_ERR_CAPABILITY_MISSING:
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    case SAO_STATUS_ERR_CANCELLED:
        return loader::SAO_PLUGINS_ERR_BUSY;
    case SAO_STATUS_ERR_ABI_MISMATCH:
        return loader::SAO_PLUGINS_ERR_ABI_MISMATCH;
    case SAO_STATUS_ERR_ACCESS_DENIED:
        return loader::SAO_PLUGINS_ERR_NOT_OWNER;
    case SAO_STATUS_ERR_ALREADY_EXISTS:
        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
    case SAO_STATUS_ERR_UNKNOWN:
    case SAO_STATUS_ERR_TIMEOUT:
    case SAO_STATUS_ERR_OS_CALL_FAILED:
    case SAO_STATUS_ERR_DEVICE_LOST:
    default:
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t mapSdkPlatformStatus(sao_sdk_status_t status) noexcept {
    switch (status) {
    case SAO_SDK_OK:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    case SAO_SDK_ERR_INVALID_ARGUMENT:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    case SAO_SDK_ERR_NOT_INITIALIZED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_INITIALIZED;
    case SAO_SDK_ERR_HANDLE_INVALID:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
    case SAO_SDK_ERR_BUFFER_TOO_SMALL:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUFFER_TOO_SMALL;
    case SAO_SDK_ERR_NOT_IMPLEMENTED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_IMPLEMENTED;
    case SAO_SDK_ERR_ABI_MISMATCH:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ABI_MISMATCH;
    case SAO_SDK_ERR_UNSUPPORTED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNSUPPORTED;
    case SAO_SDK_ERR_BUSY:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUSY;
    case SAO_SDK_ERR_ACCESS_DENIED:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ACCESS_DENIED;
    case SAO_SDK_ERR_NOT_FOUND:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_FOUND;
    case SAO_SDK_ERR_ALREADY_EXISTS:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_ALREADY_EXISTS;
    case SAO_SDK_ERR_INTERNAL:
    default:
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

bool erasePlatformTimerLocked(LauncherPlatformSession& session, uint64_t provider_token,
                              const std::shared_ptr<LauncherPlatformTimer>& timer) noexcept {
    const auto found = session.timers.find(provider_token);
    if (found == session.timers.end() || found->second != timer)
        return false;
    session.timers.erase(found);
    g_platform_timer_count.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void wakeTimerWorkerLocked(LauncherPlatformSession& session) noexcept {
    session.timer_worker_wake = true;
}

sao_sdk_status_t unregisterSdkTimer(LauncherPlatformSession& session,
                                    sao_sdk_timer_token_t sdk_token) noexcept {
    g_platform_timer_unregister_attempt_count.fetch_add(1, std::memory_order_relaxed);
    const auto injected = g_test_fail_next_timer_unregister.exchange(SAO_SDK_OK);
    if (injected != SAO_SDK_OK)
        return injected;
    return sao_sdk_timer_unregister(&session.sdk_context, sdk_token);
}

int32_t unregisterPlatformTimer(LauncherPlatformSession& session, uint64_t provider_token,
                                bool missing_is_success = false);
int32_t destroyPlatformCompositorLayerCore(LauncherPlatformSession& session,
                                           uint64_t provider_token);

void dispatchPlatformTimerCallback(const std::shared_ptr<LauncherPlatformTimer>& timer) noexcept {
    if (timer == nullptr || timer->session == nullptr)
        return;
    auto* session = timer->session;
    loader::timer_callback_fn callback = nullptr;
    void* callback_user_data = nullptr;
    {
        std::lock_guard lock(session->mutex);
        const auto found = session->timers.find(timer->provider_token);
        if (found == session->timers.end() || found->second != timer)
            return;
        if (timer->registering || !timer->published) {
            if (timer->one_shot) {
                if (timer->fired)
                    return;
                timer->fired = true;
            }
            timer->pending_fired = true;
            return;
        }
        if (timer->callback_active)
            return;
        if (timer->one_shot) {
            if (timer->fired && !timer->pending_fired)
                return;
            timer->fired = true;
        }
        timer->pending_fired = false;
        if (!session->accepting_callbacks || timer->callback == nullptr) {
            if (timer->one_shot && !timer->unregistering) {
                timer->cleanup_pending = true;
                wakeTimerWorkerLocked(*session);
            }
            session->idle.notify_all();
            return;
        }
        timer->callback_active = true;
        callback = timer->callback;
        callback_user_data = timer->callback_user_data;
        ++session->active_callbacks;
    }
    auto* previous = g_platform_callback_session;
    g_platform_callback_session = session;
    {
        LoaderCallbackLease loader_callback(session);
        if (loader_callback) {
            try {
                callback(callback_user_data);
            } catch (...) {
            }
        }
    }
    g_platform_callback_session = previous;
    {
        std::lock_guard lock(session->mutex);
        timer->callback_active = false;
        if (session->active_callbacks > 0)
            --session->active_callbacks;
        const auto found = session->timers.find(timer->provider_token);
        if (timer->one_shot && found != session->timers.end() && found->second == timer &&
            !timer->unregistering) {
            timer->cleanup_pending = true;
            wakeTimerWorkerLocked(*session);
        }
    }
    session->idle.notify_all();
}

void SAO_SDK_CALL launcherPlatformTimerCallback(sao_sdk_timer_token_t, void* user_data) {
    auto* raw_timer = static_cast<LauncherPlatformTimer*>(user_data);
    if (raw_timer == nullptr || raw_timer->session == nullptr)
        return;
    auto* session = raw_timer->session;
    std::shared_ptr<LauncherPlatformTimer> timer;
    {
        std::lock_guard lock(session->mutex);
        const auto found = session->timers.find(raw_timer->provider_token);
        if (found == session->timers.end() || found->second.get() != raw_timer)
            return;
        timer = found->second;
    }
    dispatchPlatformTimerCallback(timer);
}

void platformTimerCleanupWorker(LauncherPlatformSession* session) noexcept {
    g_platform_timer_worker_count.fetch_add(1, std::memory_order_relaxed);
    try {
        for (;;) {
            uint64_t provider_token = 0;
            {
                std::unique_lock lock(session->mutex);
                session->idle.wait(lock, [session] {
                    return session->timer_worker_stop || session->timer_worker_wake ||
                           std::any_of(session->timers.begin(), session->timers.end(),
                                       [](const auto& candidate) {
                                           const auto& timer = candidate.second;
                                           return timer->cleanup_pending && !timer->registering &&
                                                  !timer->unregistering && !timer->callback_active;
                                       });
                });
                session->timer_worker_wake = false;
                const auto found = std::find_if(
                    session->timers.begin(), session->timers.end(), [](const auto& candidate) {
                        const auto& timer = candidate.second;
                        return timer->cleanup_pending && !timer->registering &&
                               !timer->unregistering && !timer->callback_active;
                    });
                if (found != session->timers.end())
                    provider_token = found->first;
                else if (session->timer_worker_stop)
                    break;
            }
            if (provider_token != 0)
                (void)unregisterPlatformTimer(*session, provider_token, true);
        }
    } catch (...) {
    }
    g_platform_timer_worker_count.fetch_sub(1, std::memory_order_relaxed);
    session->idle.notify_all();
}

int32_t ensurePlatformTimerWorker(LauncherPlatformSession& session) noexcept {
    try {
        std::lock_guard lock(session.mutex);
        if (session.timer_cleanup_worker.joinable())
            return SAO_OK;
        session.timer_worker_stop = false;
        session.timer_worker_wake = false;
        session.timer_cleanup_worker = std::thread(platformTimerCleanupWorker, &session);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void stopPlatformTimerWorker(LauncherPlatformSession& session) noexcept {
    try {
        {
            std::lock_guard lock(session.mutex);
            session.timer_worker_stop = true;
            wakeTimerWorkerLocked(session);
        }
        session.idle.notify_all();
        if (session.timer_cleanup_worker.joinable())
            session.timer_cleanup_worker.join();
    } catch (...) {
    }
}
#endif

int32_t
    SAO_PLUGINS_CALL createPlatformSession(void*,
                                           const loader::plugin_context_platform_session_spec* spec,
                                           loader::plugin_context_platform_session_t* out_session) {
    if (out_session != nullptr)
        *out_session = nullptr;
    if (spec == nullptr || out_session == nullptr ||
        spec->struct_size < SAO_PLUGIN_CONTEXT_PLATFORM_SESSION_SPEC_V1_2_SIZE ||
        spec->plugin_id_utf8 == nullptr || spec->plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    std::unique_ptr<LauncherPlatformSession> session;
    try {
        session.reset(new (std::nothrow) LauncherPlatformSession());
        if (!session)
            return SAO_ERR_OS_CALL_FAILED;
        session->plugin_id = spec->plugin_id_utf8;
        do {
            session->session_id =
                g_platform_session_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        } while (session->session_id == 0);
        if (spec->struct_size >= sizeof(loader::plugin_context_platform_session_spec)) {
            if ((spec->enter_callback == nullptr) != (spec->leave_callback == nullptr))
                return SAO_ERR_INVALID_ARGUMENT;
            session->callback_gate_user_data = spec->callback_gate_user_data;
            session->enter_callback = spec->enter_callback;
            session->leave_callback = spec->leave_callback;
        }
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    try {
        const char* version = spec->plugin_version_utf8 == nullptr ? "" : spec->plugin_version_utf8;
        int32_t status = mapSdkStatus(
            sao_sdk_bind_context(spec->plugin_id_utf8, version, &session->sdk_context));
        if (status != SAO_OK) {
            session->sdk_ready = session->sdk_context.ctx_impl != nullptr;
            if (session->sdk_ready)
                *out_session = session.release();
            return status;
        }
        session->sdk_ready = true;
        status = mapSdkStatus(sao_sdk_context_bind_platform_services(&session->sdk_context));
        if (status != SAO_OK) {
            *out_session = session.release();
            return status;
        }
    } catch (...) {
        session->sdk_ready = session->sdk_context.ctx_impl != nullptr;
        if (session->sdk_ready)
            *out_session = session.release();
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
    *out_session = session.release();
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL
quiescePlatformSession(void*, loader::plugin_context_platform_session_t provider_session) {
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (g_platform_callback_session == session)
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        std::unique_lock lock(session->mutex);
        session->accepting_callbacks = false;
        session->idle.wait(lock, [session] { return session->active_callbacks == 0; });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
int32_t unregisterPlatformTimer(LauncherPlatformSession& session, uint64_t provider_token,
                                bool missing_is_success) {
    if (g_platform_callback_session == &session)
        return loader::SAO_PLUGINS_ERR_BUSY;
    std::shared_ptr<LauncherPlatformTimer> timer;
    sao_sdk_timer_token_t sdk_token = 0;
    {
        std::unique_lock lock(session.mutex);
        for (;;) {
            const auto found = session.timers.find(provider_token);
            if (found == session.timers.end())
                return missing_is_success ? SAO_OK : SAO_ERR_HANDLE_INVALID;
            timer = found->second;
            if (timer->registering)
                return loader::SAO_PLUGINS_ERR_BUSY;
            if (!timer->unregistering)
                break;
            session.idle.wait(lock, [&session, provider_token, &timer] {
                const auto current = session.timers.find(provider_token);
                return current == session.timers.end() || current->second != timer ||
                       !timer->unregistering;
            });
            missing_is_success = true;
        }
        timer->unregistering = true;
        timer->cleanup_pending = false;
        sdk_token = timer->sdk_token;
    }
    const int32_t status = mapSdkStatus(unregisterSdkTimer(session, sdk_token));
    {
        std::lock_guard lock(session.mutex);
        timer->unregistering = false;
        if (status == SAO_OK || status == SAO_ERR_HANDLE_INVALID)
            (void)erasePlatformTimerLocked(session, provider_token, timer);
    }
    session.idle.notify_all();
    return status == SAO_ERR_HANDLE_INVALID ? SAO_OK : status;
}

int32_t dismissPlatformNotification(LauncherPlatformSession& session, uint64_t provider_token) {
    sao_sdk_notify_token_t sdk_token = 0;
    {
        std::lock_guard lock(session.mutex);
        const auto found = session.notifications.find(provider_token);
        if (found == session.notifications.end())
            return SAO_ERR_HANDLE_INVALID;
        sdk_token = found->second;
    }
    const int32_t status = mapSdkStatus(sao_sdk_notify_dismiss(&session.sdk_context, sdk_token));
    if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
        return status;
    std::lock_guard lock(session.mutex);
    session.notifications.erase(provider_token);
    return SAO_OK;
}
#endif

int32_t SAO_PLUGINS_CALL
destroyPlatformSession(void*, loader::plugin_context_platform_session_t provider_session) {
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (g_platform_callback_session == session)
        return loader::SAO_PLUGINS_ERR_BUSY;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    try {
        for (;;) {
            uint64_t token = 0;
            {
                std::lock_guard lock(session->mutex);
                if (session->timers.empty())
                    break;
                token = session->timers.begin()->first;
            }
            const int32_t status = unregisterPlatformTimer(*session, token);
            if (status != SAO_OK)
                return status;
        }
        stopPlatformTimerWorker(*session);
        for (;;) {
            uint64_t token = 0;
            {
                std::lock_guard lock(session->mutex);
                if (session->notifications.empty())
                    break;
                token = session->notifications.begin()->first;
            }
            const int32_t status = dismissPlatformNotification(*session, token);
            if (status != SAO_OK)
                return status;
        }
        for (;;) {
            uint64_t token = 0;
            {
                std::lock_guard lock(session->mutex);
                if (session->compositor_layers.empty())
                    break;
                token = session->compositor_layers.begin()->first;
            }
            const int32_t status = destroyPlatformCompositorLayerCore(*session, token);
            if (status != SAO_STATUS_OK)
                return mapUiLoaderStatus(static_cast<sao_status_t>(status));
        }
        if (session->sdk_ready) {
            const int32_t status = mapSdkStatus(sao_sdk_context_try_destroy(&session->sdk_context));
            if (status != SAO_OK)
                return status;
            session->sdk_ready = false;
        }
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
    delete session;
    return SAO_OK;
}

#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
int32_t SAO_PLUGINS_CALL registerPlatformTimer(
    void*, loader::plugin_context_platform_session_t provider_session, double seconds,
    bool one_shot, loader::timer_callback_fn callback, void* callback_user_data,
    loader::plugin_context_platform_token_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || callback == nullptr || out_provider_token == nullptr ||
        !std::isfinite(seconds) || seconds <= 0.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const double milliseconds = seconds * 1000.0;
    if (!std::isfinite(milliseconds) ||
        milliseconds > static_cast<double>((std::numeric_limits<uint32_t>::max)())) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const uint32_t interval_ms =
        (std::max)(uint32_t{1}, static_cast<uint32_t>(std::ceil(milliseconds)));
    try {
        auto timer = std::make_shared<LauncherPlatformTimer>();
        timer->session = session;
        timer->callback = callback;
        timer->callback_user_data = callback_user_data;
        timer->one_shot = one_shot;
        const int32_t worker_status = ensurePlatformTimerWorker(*session);
        if (worker_status != SAO_OK)
            return worker_status;
        uint64_t provider_token = 0;
        {
            std::lock_guard lock(session->mutex);
            if (!session->accepting_callbacks)
                return loader::SAO_PLUGINS_ERR_BUSY;
            provider_token = nextPlatformTokenLocked(*session);
            timer->provider_token = provider_token;
            const auto [_, inserted] = session->timers.emplace(provider_token, timer);
            if (!inserted)
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            g_platform_timer_count.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_test_fire_timer_during_register.exchange(false))
            launcherPlatformTimerCallback(0, timer.get());
        sao_sdk_timer_token_t sdk_token = 0;
        const int32_t status = mapSdkStatus(
            sao_sdk_timer_register(&session->sdk_context, interval_ms,
                                   launcherPlatformTimerCallback, timer.get(), &sdk_token));
        if (status != SAO_OK) {
            std::lock_guard lock(session->mutex);
            timer->registering = false;
            (void)erasePlatformTimerLocked(*session, provider_token, timer);
            return status;
        }
        bool accepting = false;
        {
            std::lock_guard lock(session->mutex);
            timer->sdk_token = sdk_token;
            timer->registering = false;
            accepting = session->accepting_callbacks;
            if (accepting) {
                *out_provider_token = provider_token;
                timer->published = true;
            }
        }
        if (!accepting) {
            const int32_t rollback_status = unregisterPlatformTimer(*session, provider_token);
            return rollback_status == SAO_OK ? loader::SAO_PLUGINS_ERR_BUSY : rollback_status;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL
unregisterPlatformTimer(void*, loader::plugin_context_platform_session_t provider_session,
                        loader::plugin_context_platform_token_t provider_token) {
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || provider_token == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        return unregisterPlatformTimer(*session, provider_token);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

uint32_t notificationColor(std::string_view kind) noexcept {
    if (kind == "error")
        return 0xffd84a4aU;
    if (kind == "warning" || kind == "warn")
        return 0xffffb020U;
    if (kind == "success")
        return 0xff35c46aU;
    return 0xff4aa3ffU;
}

int32_t SAO_PLUGINS_CALL showPlatformNotification(
    void*, loader::plugin_context_platform_session_t provider_session, const char* title_utf8,
    const char* message_utf8, double duration_s, const char* kind_utf8,
    loader::plugin_context_platform_token_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || message_utf8 == nullptr || out_provider_token == nullptr ||
        !std::isfinite(duration_s) || duration_s <= 0.0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const double milliseconds = duration_s * 1000.0;
    if (!std::isfinite(milliseconds) ||
        milliseconds > static_cast<double>((std::numeric_limits<uint32_t>::max)())) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::string text;
        if (title_utf8 != nullptr && title_utf8[0] != '\0') {
            text.assign(title_utf8);
            text.append(": ");
        }
        text.append(message_utf8);
        const std::string_view kind = kind_utf8 == nullptr ? "info" : kind_utf8;
        SaoSdkNotifySpec spec{};
        spec.text_utf8 = text.c_str();
        spec.duration_ms = (std::max)(uint32_t{1}, static_cast<uint32_t>(std::ceil(milliseconds)));
        spec.argb_color = notificationColor(kind);
        uint64_t provider_token = 0;
        sao_sdk_notify_token_t* sdk_token_slot = nullptr;
        {
            std::lock_guard lock(session->mutex);
            if (!session->accepting_callbacks)
                return loader::SAO_PLUGINS_ERR_BUSY;
            provider_token = nextPlatformTokenLocked(*session);
            const auto [entry, inserted] = session->notifications.emplace(provider_token, 0);
            if (!inserted)
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            sdk_token_slot = &entry->second;
        }
        const int32_t status =
            mapSdkStatus(sao_sdk_notify_show(&session->sdk_context, &spec, sdk_token_slot));
        if (status != SAO_OK) {
            std::lock_guard lock(session->mutex);
            session->notifications.erase(provider_token);
            return status;
        }
        bool accepting = false;
        {
            std::lock_guard lock(session->mutex);
            accepting = session->accepting_callbacks;
        }
        if (!accepting) {
            const int32_t rollback_status = dismissPlatformNotification(*session, provider_token);
            return rollback_status == SAO_OK ? loader::SAO_PLUGINS_ERR_BUSY : rollback_status;
        }
        *out_provider_token = provider_token;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL
dismissPlatformNotification(void*, loader::plugin_context_platform_session_t provider_session,
                            loader::plugin_context_platform_token_t provider_token) {
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || provider_token == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        return dismissPlatformNotification(*session, provider_token);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

template <typename Callback>
void dispatchCompositorInput(LauncherPlatformCompositorLayer* raw_layer,
                             Callback&& invoke) noexcept {
    if (raw_layer == nullptr || raw_layer->session == nullptr)
        return;
    auto* session = raw_layer->session;
    std::shared_ptr<LauncherPlatformCompositorLayer> layer;
    {
        std::lock_guard lock(session->mutex);
        const auto found = session->compositor_layers.find(raw_layer->provider_token);
        if (found == session->compositor_layers.end() || found->second.get() != raw_layer ||
            !session->accepting_callbacks) {
            return;
        }
        layer = found->second;
        ++session->active_callbacks;
    }
    auto* previous = g_platform_callback_session;
    g_platform_callback_session = session;
    try {
        invoke(*layer);
    } catch (...) {
    }
    g_platform_callback_session = previous;
    {
        std::lock_guard lock(session->mutex);
        if (session->active_callbacks > 0)
            --session->active_callbacks;
    }
    session->idle.notify_all();
}

template <typename Callback, typename Invoke>
void invokeCompositorPluginCallback(LauncherPlatformCompositorLayer& layer,
                                    Callback LauncherPlatformCompositorLayer::* callback_member,
                                    Invoke&& invoke) {
    Callback callback = nullptr;
    void* callback_user_data = nullptr;
    uint64_t generation = 0;
    {
        std::lock_guard lock(layer.callback_mutex);
        callback = layer.*callback_member;
        if (callback == nullptr)
            return;
        callback_user_data = layer.callback_user_data;
        generation = layer.callback_generation;
        ++layer.callbacks_by_generation[generation];
    }
    ActiveLauncherCompositorCallback marker{&layer, generation, g_active_compositor_callback};
    g_active_compositor_callback = &marker;
    {
        LoaderCallbackLease loader_callback(layer.session);
        if (loader_callback) {
            try {
                invoke(callback, callback_user_data);
            } catch (...) {
            }
        }
    }
    g_active_compositor_callback = marker.previous;
    {
        std::lock_guard lock(layer.callback_mutex);
        const auto found = layer.callbacks_by_generation.find(generation);
        if (found != layer.callbacks_by_generation.end() && --found->second == 0)
            layer.callbacks_by_generation.erase(found);
    }
    layer.callback_idle.notify_all();
}

void SAO_UI_CALL launcherCompositorCursor(float x, float y, void* user_data) {
    dispatchCompositorInput(
        static_cast<LauncherPlatformCompositorLayer*>(user_data), [x, y](auto& layer) {
            invokeCompositorPluginCallback(layer, &LauncherPlatformCompositorLayer::cursor_pos,
                                           [x, y](auto callback, void* callback_user_data) {
                                               callback(x, y, callback_user_data);
                                           });
        });
}

void SAO_UI_CALL launcherCompositorLeave(void* user_data) {
    dispatchCompositorInput(
        static_cast<LauncherPlatformCompositorLayer*>(user_data), [](auto& layer) {
            invokeCompositorPluginCallback(
                layer, &LauncherPlatformCompositorLayer::cursor_leave,
                [](auto callback, void* callback_user_data) { callback(callback_user_data); });
        });
}

void SAO_UI_CALL launcherCompositorButton(int32_t button, int32_t action, int32_t, float, float,
                                          void* user_data) {
    dispatchCompositorInput(
        static_cast<LauncherPlatformCompositorLayer*>(user_data), [button, action](auto& layer) {
            invokeCompositorPluginCallback(
                layer, &LauncherPlatformCompositorLayer::mouse_button,
                [button, action](auto callback, void* callback_user_data) {
                    callback(static_cast<uint32_t>(button), action != 0, callback_user_data);
                });
        });
}

void SAO_UI_CALL launcherCompositorScroll(float dx, float dy, void* user_data) {
    dispatchCompositorInput(
        static_cast<LauncherPlatformCompositorLayer*>(user_data), [dx, dy](auto& layer) {
            invokeCompositorPluginCallback(layer, &LauncherPlatformCompositorLayer::scroll,
                                           [dx, dy](auto callback, void* callback_user_data) {
                                               callback(dx, dy, callback_user_data);
                                           });
        });
}

int32_t getCentralCompositor(sao_ui_compositor_handle_t* out_compositor) noexcept {
    if (out_compositor == nullptr)
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    *out_compositor = nullptr;
    void* raw = nullptr;
    const sao_sdk_status_t status = sao_sdk_platform_get_ui_compositor(&raw);
    if (status != SAO_SDK_OK)
        return mapSdkPlatformStatus(status);
    *out_compositor = static_cast<sao_ui_compositor_handle_t>(raw);
    if (*out_compositor == nullptr)
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_NOT_INITIALIZED;
    return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
}

bool boundedCompositorLayerName(const char* value, size_t* out_length) noexcept {
    if (value == nullptr || out_length == nullptr)
        return false;
#if defined(_MSC_VER)
    __try {
#endif
        size_t length = 0;
        while (length <= SAO_PLUGIN_CONTEXT_COMPOSITOR_LAYER_NAME_MAX_BYTES &&
               value[length] != '\0') {
            ++length;
        }
        if (length == 0 || length > SAO_PLUGIN_CONTEXT_COMPOSITOR_LAYER_NAME_MAX_BYTES)
            return false;
        *out_length = length;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

std::shared_ptr<LauncherPlatformCompositorLayer>
findCompositorLayer(LauncherPlatformSession& session, uint64_t provider_token) {
    std::lock_guard lock(session.mutex);
    const auto found = session.compositor_layers.find(provider_token);
    return found == session.compositor_layers.end()
               ? std::shared_ptr<LauncherPlatformCompositorLayer>()
               : found->second;
}

int32_t SAO_PLUGINS_CALL
createPlatformCompositorLayer(void*, loader::plugin_context_platform_session_t provider_session,
                              const loader::plugin_context_compositor_layer_spec* spec,
                              loader::plugin_context_platform_token_t* out_provider_token) {
    if (out_provider_token != nullptr)
        *out_provider_token = 0;
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || spec == nullptr || out_provider_token == nullptr ||
        spec->struct_size < sizeof(*spec) || spec->width == 0 || spec->height == 0 ||
        spec->width > static_cast<uint32_t>(INT32_MAX) ||
        spec->height > static_cast<uint32_t>(INT32_MAX) ||
        spec->target_fps > static_cast<uint32_t>(INT32_MAX)) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        size_t local_name_length = 0;
        if (!boundedCompositorLayerName(spec->name_utf8, &local_name_length))
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
        (void)local_name_length;
        sao_ui_compositor_handle_t compositor = nullptr;
        int32_t status = getCentralCompositor(&compositor);
        if (status != loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK)
            return status;

        auto layer = std::make_shared<LauncherPlatformCompositorLayer>();
        layer->session = session;
        layer->compositor = compositor;
        layer->width = spec->width;
        layer->height = spec->height;
        {
            std::lock_guard lock(session->mutex);
            if (!session->accepting_callbacks)
                return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUSY;
            layer->provider_token = nextPlatformTokenLocked(*session);
        }
        const std::string layer_name = "plugin." + std::to_string(session->session_id) + "." +
                                       std::to_string(layer->provider_token);

        SaoLayerConfig config{};
        config.struct_size = sizeof(SaoLayerConfig);
        config.name_utf8 = layer_name.c_str();
        config.x = spec->x;
        config.y = spec->y;
        config.width = static_cast<int32_t>(spec->width);
        config.height = static_cast<int32_t>(spec->height);
        config.z_order = spec->z;
        config.click_through = spec->click_through;
        config.rect_hit = false;
        config.bgra_swizzle = true;
        config.high_fps = spec->high_fps;
        config.target_fps = static_cast<int32_t>(spec->target_fps);
        sao_status_t ui_status = sao_ui_layer_create(compositor, &config, &layer->layer);
        if (ui_status != SAO_STATUS_OK)
            return mapUiStatus(ui_status);
        ui_status = sao_ui_layer_set_input_callbacks(
            layer->layer, &launcherCompositorCursor, &launcherCompositorLeave,
            &launcherCompositorButton, &launcherCompositorScroll, layer.get());
        if (ui_status != SAO_STATUS_OK) {
            sao_ui_layer_destroy(layer->layer);
            return mapUiStatus(ui_status);
        }

        bool published = false;
        {
            std::lock_guard lock(session->mutex);
            if (session->accepting_callbacks) {
                published = session->compositor_layers.emplace(layer->provider_token, layer).second;
            }
        }
        if (!published) {
            (void)sao_ui_layer_set_input_callbacks(layer->layer, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr);
            sao_ui_layer_destroy(layer->layer);
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_BUSY;
        }
        *out_provider_token = layer->provider_token;
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    } catch (...) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

int32_t SAO_PLUGINS_CALL uploadPlatformCompositorFrame(
    void*, loader::plugin_context_platform_session_t provider_session,
    loader::plugin_context_platform_token_t provider_token, const uint8_t* bgra_bytes,
    size_t bytes_len, uint32_t width, uint32_t height) {
    try {
        auto* session = static_cast<LauncherPlatformSession*>(provider_session);
        if (session == nullptr || provider_token == 0 || bgra_bytes == nullptr || width == 0 ||
            height == 0 || width > UINT32_MAX / 4U || static_cast<size_t>(width) > SIZE_MAX / 4U ||
            static_cast<size_t>(height) > SIZE_MAX / (static_cast<size_t>(width) * 4U) ||
            bytes_len != static_cast<size_t>(width) * height * 4U) {
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
        }
        const auto layer = findCompositorLayer(*session, provider_token);
        if (layer == nullptr)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
        if (layer->width != width || layer->height != height)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
        return mapUiStatus(
            sao_ui_layer_update_bgra(layer->layer, bgra_bytes, width, height, width * 4U));
    } catch (...) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

int32_t SAO_PLUGINS_CALL setPlatformCompositorLayerPosition(
    void*, loader::plugin_context_platform_session_t provider_session,
    loader::plugin_context_platform_token_t provider_token, int32_t x, int32_t y) {
    try {
        auto* session = static_cast<LauncherPlatformSession*>(provider_session);
        if (session == nullptr || provider_token == 0)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
        const auto layer = findCompositorLayer(*session, provider_token);
        if (layer == nullptr)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
        return mapUiStatus(sao_ui_layer_set_position(layer->layer, x, y));
    } catch (...) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

int32_t SAO_PLUGINS_CALL setPlatformCompositorLayerVisible(
    void*, loader::plugin_context_platform_session_t provider_session,
    loader::plugin_context_platform_token_t provider_token, bool visible) {
    try {
        auto* session = static_cast<LauncherPlatformSession*>(provider_session);
        if (session == nullptr || provider_token == 0)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
        const auto layer = findCompositorLayer(*session, provider_token);
        if (layer == nullptr)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
        return mapUiStatus(sao_ui_layer_set_visible(layer->layer, visible));
    } catch (...) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

int32_t SAO_PLUGINS_CALL
setPlatformCompositorLayerInput(void*, loader::plugin_context_platform_session_t provider_session,
                                loader::plugin_context_platform_token_t provider_token,
                                const loader::plugin_context_compositor_input_spec* spec) {
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || provider_token == 0 || spec == nullptr ||
        spec->struct_size < sizeof(*spec)) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        const auto layer = findCompositorLayer(*session, provider_token);
        if (layer == nullptr)
            return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_HANDLE_INVALID;
        const sao_status_t central_status = sao_ui_layer_set_input_callbacks(
            layer->layer, &launcherCompositorCursor, &launcherCompositorLeave,
            &launcherCompositorButton, &launcherCompositorScroll, layer.get());
        if (central_status != SAO_STATUS_OK)
            return mapUiStatus(central_status);
        std::unique_lock callback_lock(layer->callback_mutex);
        std::vector<uint64_t> generations_to_drain;
        generations_to_drain.reserve(layer->callbacks_by_generation.size());
        for (const auto& [generation, count] : layer->callbacks_by_generation) {
            const size_t active_here = compositorCallbackActiveCount(layer.get(), generation);
            if (active_here != 0 && count > active_here)
                return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_CANCELLED;
            if (active_here == 0 && count != 0)
                generations_to_drain.push_back(generation);
        }
        if (!generations_to_drain.empty()) {
            layer->callback_idle.wait(callback_lock, [&] {
                return std::ranges::none_of(generations_to_drain, [&](uint64_t generation) {
                    const auto found = layer->callbacks_by_generation.find(generation);
                    return found != layer->callbacks_by_generation.end() && found->second != 0;
                });
            });
        }
        ++layer->callback_generation;
        if (layer->callback_generation == 0)
            layer->callback_generation = 1;
        layer->cursor_pos = spec->cursor_pos;
        layer->mouse_button = spec->mouse_button;
        layer->cursor_leave = spec->cursor_leave;
        layer->scroll = spec->scroll;
        layer->callback_user_data = spec->user_data;
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_OK;
    } catch (...) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}

int32_t destroyPlatformCompositorLayerCore(LauncherPlatformSession& session,
                                           uint64_t provider_token) {
    if (g_platform_callback_session == &session)
        return SAO_STATUS_ERR_CANCELLED;
    const auto layer = findCompositorLayer(session, provider_token);
    if (layer == nullptr)
        return SAO_STATUS_ERR_HANDLE_INVALID;
    sao_status_t callback_status = sao_ui_layer_set_visible(layer->layer, false);
    if (callback_status != SAO_STATUS_OK)
        return callback_status;
    callback_status =
        sao_ui_layer_set_input_callbacks(layer->layer, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (callback_status != SAO_STATUS_OK)
        return callback_status;
    sao_ui_layer_destroy(layer->layer);
    std::lock_guard lock(session.mutex);
    const auto found = session.compositor_layers.find(provider_token);
    if (found != session.compositor_layers.end() && found->second == layer)
        session.compositor_layers.erase(found);
    return SAO_STATUS_OK;
}

int32_t SAO_PLUGINS_CALL
destroyPlatformCompositorLayer(void*, loader::plugin_context_platform_session_t provider_session,
                               loader::plugin_context_platform_token_t provider_token) {
    auto* session = static_cast<LauncherPlatformSession*>(provider_session);
    if (session == nullptr || provider_token == 0)
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_INVALID_ARGUMENT;
    try {
        return mapUiStatus(destroyPlatformCompositorLayerCore(*session, provider_token));
    } catch (...) {
        return loader::SAO_PLUGIN_CONTEXT_PLATFORM_STATUS_ERR_UNKNOWN;
    }
}
#endif

loader::plugin_context_platform_provider makePlatformProvider() noexcept {
    loader::plugin_context_platform_provider provider{};
    provider.abi_version = SAO_PLUGIN_CONTEXT_PLATFORM_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &g_platform_provider_owner;
    provider.retain = retainProviderOwner;
    provider.release = releaseProviderOwner;
    provider.create_session = createPlatformSession;
    provider.quiesce_session = quiescePlatformSession;
    provider.destroy_session = destroyPlatformSession;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    provider.register_timer = registerPlatformTimer;
    provider.unregister_timer = unregisterPlatformTimer;
    provider.show_notify = showPlatformNotification;
    provider.dismiss_notify = dismissPlatformNotification;
    provider.create_compositor_layer = createPlatformCompositorLayer;
    provider.upload_compositor_frame = uploadPlatformCompositorFrame;
    provider.set_compositor_layer_position = setPlatformCompositorLayerPosition;
    provider.set_compositor_layer_visible = setPlatformCompositorLayerVisible;
    provider.set_compositor_layer_input = setPlatformCompositorLayerInput;
    provider.destroy_compositor_layer = destroyPlatformCompositorLayer;
#endif
    return provider;
}

struct DependencyPathLease {
    std::wstring path;
    DLL_DIRECTORY_COOKIE cookie = nullptr;
};

struct LauncherDependencySession {
    std::mutex mutex;
    std::vector<DependencyPathLease> paths;
};

int32_t SAO_PLUGINS_CALL createDependencySession(void*, const loader::deps_session_spec* spec,
                                                 void** out_provider_session) {
    if (out_provider_session != nullptr)
        *out_provider_session = nullptr;
    if (spec == nullptr || out_provider_session == nullptr ||
        spec->struct_size < sizeof(loader::deps_session_spec) || spec->plugin_id_utf8 == nullptr ||
        spec->plugin_id_utf8[0] == '\0' || spec->plugin_dir == nullptr ||
        spec->plugin_dir[0] == L'\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    LauncherDependencySession* session = nullptr;
    try {
        session = new (std::nothrow) LauncherDependencySession();
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (session == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    *out_provider_session = session;
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL attachDependencyPath(void*, void* provider_session,
                                              const wchar_t* absolute_dir) {
    auto* session = static_cast<LauncherDependencySession*>(provider_session);
    if (session == nullptr || absolute_dir == nullptr || absolute_dir[0] == L'\0')
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        const std::filesystem::path path(absolute_dir);
        std::error_code error;
        if (!path.is_absolute() || !std::filesystem::is_directory(path, error) || error)
            return SAO_ERR_INVALID_ARGUMENT;
        std::wstring normalized = path.lexically_normal().wstring();
        std::lock_guard lock(session->mutex);
        session->paths.reserve(session->paths.size() + 1);
        session->paths.push_back({std::move(normalized), nullptr});
        const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(session->paths.back().path.c_str());
        if (cookie == nullptr) {
            session->paths.pop_back();
            return SAO_ERR_OS_CALL_FAILED;
        }
        session->paths.back().cookie = cookie;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL restoreDependencyPath(void*, void* provider_session,
                                               const wchar_t* absolute_dir) {
    auto* session = static_cast<LauncherDependencySession*>(provider_session);
    if (session == nullptr || absolute_dir == nullptr || absolute_dir[0] == L'\0')
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        const std::wstring normalized =
            std::filesystem::path(absolute_dir).lexically_normal().wstring();
        std::lock_guard lock(session->mutex);
        const auto found = std::find_if(session->paths.rbegin(), session->paths.rend(),
                                        [&normalized](const DependencyPathLease& candidate) {
                                            return candidate.path == normalized;
                                        });
        if (found == session->paths.rend())
            return SAO_ERR_HANDLE_INVALID;
        if (!RemoveDllDirectory(found->cookie))
            return SAO_ERR_OS_CALL_FAILED;
        session->paths.erase(std::prev(found.base()));
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL closeDependencySession(void*, void* provider_session) {
    auto* session = static_cast<LauncherDependencySession*>(provider_session);
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        {
            std::lock_guard lock(session->mutex);
            if (!session->paths.empty())
                return loader::SAO_PLUGINS_ERR_BUSY;
        }
        delete session;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

loader::deps_provider makeDependencyProvider() noexcept {
    loader::deps_provider provider{};
    provider.abi_version = SAO_PLUGIN_DEPS_PROVIDER_ABI_VERSION;
    provider.struct_size = sizeof(provider);
    provider.user_data = &g_deps_provider_owner;
    provider.retain = retainProviderOwner;
    provider.release = releaseProviderOwner;
    provider.create_session = createDependencySession;
    provider.attach_path = attachDependencyPath;
    provider.restore_path = restoreDependencyPath;
    provider.close_session = closeDependencySession;
    return provider;
}

class ProviderOperationGuard final {
  public:
    explicit ProviderOperationGuard(sao_plugins_registry* registry) noexcept
        : registry_(g_active_operation_registry == nullptr ? registry : nullptr) {
        if (registry_ != nullptr)
            g_active_operation_registry = registry_;
    }

    ~ProviderOperationGuard() {
        if (registry_ != nullptr)
            g_active_operation_registry = nullptr;
    }

    ProviderOperationGuard(const ProviderOperationGuard&) = delete;
    ProviderOperationGuard& operator=(const ProviderOperationGuard&) = delete;

    bool acquired() const noexcept {
        return registry_ != nullptr;
    }

  private:
    sao_plugins_registry* registry_ = nullptr;
};

class RegistryLease final {
  public:
    RegistryLease() = default;
    ~RegistryLease() {
        release();
    }

    RegistryLease(const RegistryLease&) = delete;
    RegistryLease& operator=(const RegistryLease&) = delete;

    sao_status_t acquire(sao_plugins_registry* shell) noexcept {
        if (shell == nullptr)
            return SAO_STATUS_INVALID_ARGUMENT;
        try {
            std::lock_guard lock(shell->mutex);
            if (shell->retiring || shell->retired || !shell->body) {
                return SAO_STATUS_INTERNAL;
            }
            shell_ = shell;
            body_ = shell->body;
            ++shell_->active_calls;
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_INTERNAL;
        }
    }

    sao_plugins_registry_body& body() const noexcept {
        return *body_;
    }

    uint32_t active_calls() const noexcept {
        if (shell_ == nullptr)
            return 0;
        std::lock_guard lock(shell_->mutex);
        return shell_->active_calls;
    }

  private:
    void release() noexcept {
        if (shell_ == nullptr)
            return;
        {
            std::lock_guard lock(shell_->mutex);
            if (shell_->active_calls > 0)
                --shell_->active_calls;
        }
        shell_->idle.notify_all();
        body_.reset();
        shell_ = nullptr;
    }

    sao_plugins_registry* shell_ = nullptr;
    std::shared_ptr<sao_plugins_registry_body> body_;
};

bool configuredPathsExist(const sao::launcher::PluginsProviderConfiguration& configuration) {
    std::error_code error;
    for (const auto& root : configuration.roots) {
        if (!std::filesystem::is_directory(root, error) || error)
            return false;
    }
    for (const auto& root : configuration.user_roots) {
        error.clear();
        if (!std::filesystem::is_directory(root, error) || error)
            return false;
    }
    for (const auto& manifest : configuration.manifests) {
        error.clear();
        if (!std::filesystem::is_regular_file(manifest, error) || error) {
            return false;
        }
    }
    return true;
}

bool loadConfiguredManifest(const std::wstring& manifest_path, loader::plugin_manifest& manifest) {
    if (loader::sao_plugins_manifest_load_from_file(manifest_path.c_str(), &manifest) != SAO_OK ||
        loader::validate_manifest(manifest) != SAO_OK) {
        return false;
    }
    const auto directory = std::filesystem::path(manifest_path).parent_path();
    std::error_code error;
    if (manifest.native_entry.empty() &&
        (!std::filesystem::is_regular_file(directory / pathFromUtf8(manifest.entry), error) ||
         error)) {
        return false;
    }
    if (!manifest.native_entry.empty()) {
        error.clear();
        if (!std::filesystem::is_regular_file(directory / pathFromUtf8(manifest.native_entry),
                                              error) ||
            error) {
            return false;
        }
    }
    return true;
}

bool addManifest(sao_plugins_registry_body& owned, const loader::plugin_manifest& manifest,
                 std::unordered_set<std::string>& ids) {
    if (!ids.insert(manifest.plugin_id).second)
        return false;
    loader::plugin_handle_t handle = nullptr;
    if (loader::sao_plugins_registry_add_plugin(owned.registry, &manifest, &handle) != SAO_OK ||
        handle == nullptr) {
        return false;
    }
    owned.handles.push_back(handle);
    owned.autostart.push_back(manifest.enabled);
    owned.manifests.push_back(manifest);
    return true;
}

int32_t registerProductionProviders(sao_plugins_registry_body& owned) noexcept {
    try {
        const auto platform_provider = makePlatformProvider();
        int32_t status = loader::sao_plugins_ctx_register_platform_provider(&platform_provider);
        if (status != SAO_OK)
            return status;
        owned.platform_provider_owned = true;

        const auto dependency_provider = makeDependencyProvider();
        status = loader::sao_plugins_deps_register_provider(&dependency_provider);
        if (status != SAO_OK)
            return status;
        owned.deps_provider_owned = true;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t registerHostAdapters(
    [[maybe_unused]] sao_plugins_registry_body& owned,
    [[maybe_unused]] const sao::launcher::PluginsProviderConfiguration& configuration) noexcept {
    [[maybe_unused]] int32_t status = SAO_OK;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    status = sao::plugins::emma_host::sao_plugins_emma_register_loader_adapter(&owned.emma_owner);
    if (status != SAO_OK)
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    sao::plugins::angel_host::as_host_config angel_host_config{};
    status = sao::plugins::angel_host::sao_plugins_ashost_register_loader_adapter(
        &angel_host_config, &owned.angel_owner);
    if (status != SAO_OK)
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    sao::plugins::lua_host::lua_host_config lua_host_config{};
    status = sao::plugins::lua_host::sao_plugins_luahost_register_loader_adapter(&lua_host_config,
                                                                                 &owned.lua_owner);
    if (status != SAO_OK)
        return status;
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    bool available = false;
    status = sao::plugins::csharp_host::sao_plugins_cshost_is_available(&available);
    if (status != SAO_OK)
        return status;
    if (available) {
        sao::plugins::csharp_host::cs_host_config csharp_host_config{};
        status = sao::plugins::csharp_host::sao_plugins_cshost_register_loader_adapter(
            &csharp_host_config, &owned.csharp_owner);
        if (status != SAO_OK)
            return status;
    }
#endif
    return SAO_OK;
}

bool needsLifecycleUnload(loader::plugin_handle_t handle) noexcept {
    const auto state = loader::sao_plugins_lifecycle_state(handle);
    if (state == loader::lifecycle_state::discovered ||
        state == loader::lifecycle_state::unloaded) {
        return false;
    }
    if (state != loader::lifecycle_state::failed)
        return true;
    loader::plugin_context_t* context = nullptr;
    return loader::sao_plugins_lifecycle_get_context(handle, &context) == SAO_OK &&
           context != nullptr;
}

struct ReloadTarget {
    loader::plugin_handle_t handle = nullptr;
    loader::lifecycle_state original_state = loader::lifecycle_state::unknown;
};

bool isLoadedState(loader::lifecycle_state state) noexcept {
    return state == loader::lifecycle_state::loaded_active ||
           state == loader::lifecycle_state::loaded_disabled;
}

sao_status_t firstFailure(sao_status_t current, sao_status_t candidate) noexcept {
    return current == SAO_STATUS_OK && candidate != SAO_STATUS_OK ? candidate : current;
}

sao_status_t
restoreReloadTargets(const std::vector<ReloadTarget>& targets,
                     const std::unordered_set<loader::plugin_handle_t>& affected) noexcept {
    sao_status_t aggregate = SAO_STATUS_OK;
    for (const auto& target : targets) {
        if (!affected.contains(target.handle)) {
            continue;
        }
        auto current = loader::sao_plugins_lifecycle_state(target.handle);
        if (current == target.original_state) {
            continue;
        }

        if (!isLoadedState(current)) {
            if (needsLifecycleUnload(target.handle)) {
                const sao_status_t unload_status =
                    loader::sao_plugins_lifecycle_unload(target.handle);
                aggregate = firstFailure(aggregate, unload_status);
                if (unload_status != SAO_OK) {
                    continue;
                }
            }
            const sao_status_t load_status = loader::sao_plugins_lifecycle_load(target.handle);
            aggregate = firstFailure(aggregate, load_status);
            if (load_status != SAO_OK) {
                continue;
            }
            current = loader::sao_plugins_lifecycle_state(target.handle);
        }

        if (target.original_state == loader::lifecycle_state::loaded_active &&
            current != loader::lifecycle_state::loaded_active) {
            aggregate =
                firstFailure(aggregate, loader::sao_plugins_lifecycle_enable(target.handle));
        } else if (target.original_state == loader::lifecycle_state::loaded_disabled &&
                   current != loader::lifecycle_state::loaded_disabled) {
            aggregate =
                firstFailure(aggregate, loader::sao_plugins_lifecycle_disable(target.handle));
        }
    }

    for (const auto& target : targets) {
        if (affected.contains(target.handle) &&
            loader::sao_plugins_lifecycle_state(target.handle) != target.original_state) {
            aggregate = firstFailure(aggregate, SAO_STATUS_INTERNAL);
        }
    }
    return aggregate;
}

int32_t unregisterHostAdapters([[maybe_unused]] sao_plugins_registry_body& owned) noexcept {
    [[maybe_unused]] int32_t status = SAO_OK;
#if defined(SAO_LAUNCHER_PROVIDER_HAS_CSHARP)
    if (owned.csharp_owner != nullptr) {
        status = sao::plugins::csharp_host::sao_plugins_cshost_unregister_loader_adapter(
            owned.csharp_owner);
        if (status != SAO_OK)
            return status;
        owned.csharp_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_LUA)
    if (owned.lua_owner != nullptr) {
        status =
            sao::plugins::lua_host::sao_plugins_luahost_unregister_loader_adapter(owned.lua_owner);
        if (status != SAO_OK)
            return status;
        owned.lua_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_ANGEL)
    if (owned.angel_owner != nullptr) {
        status = sao::plugins::angel_host::sao_plugins_ashost_unregister_loader_adapter(
            owned.angel_owner);
        if (status != SAO_OK)
            return status;
        owned.angel_owner = nullptr;
    }
#endif
#if defined(SAO_LAUNCHER_PROVIDER_HAS_EMMA)
    if (owned.emma_owner != nullptr) {
        status =
            sao::plugins::emma_host::sao_plugins_emma_unregister_loader_adapter(owned.emma_owner);
        if (status != SAO_OK)
            return status;
        owned.emma_owner = nullptr;
    }
#endif
    return SAO_OK;
}

int32_t unregisterProductionProviders(sao_plugins_registry_body& owned) noexcept {
    if (owned.deps_provider_owned) {
        const int32_t status = loader::sao_plugins_deps_unregister_provider();
        if (status != SAO_OK)
            return status;
        owned.deps_provider_owned = false;
    }
    if (owned.platform_provider_owned) {
        const int32_t status = loader::sao_plugins_ctx_unregister_platform_provider();
        if (status != SAO_OK)
            return status;
        owned.platform_provider_owned = false;
    }
    return SAO_OK;
}

int32_t rollbackRegistry(sao_plugins_registry_body& owned) noexcept {
    while (!owned.handles.empty()) {
        const auto handle = owned.handles.back();
        const auto lifecycle_state = loader::sao_plugins_lifecycle_state(handle);
        if (needsLifecycleUnload(handle) || lifecycle_state == loader::lifecycle_state::failed) {
            const int32_t unload_status = loader::sao_plugins_lifecycle_unload(handle);
            if (unload_status != SAO_OK && !(lifecycle_state == loader::lifecycle_state::failed &&
                                             unload_status == loader::SAO_PLUGINS_ERR_BUSY)) {
                return unload_status;
            }
        }
        const int32_t remove_status = loader::sao_plugins_registry_remove(owned.registry, handle);
        if (remove_status != SAO_OK)
            return remove_status;
        owned.handles.pop_back();
        owned.autostart.pop_back();
        owned.manifests.pop_back();
    }
    const int32_t adapter_status = unregisterHostAdapters(owned);
    return adapter_status == SAO_OK ? unregisterProductionProviders(owned) : adapter_status;
}

sao_status_t rollbackDiscoveryFailure(std::unique_ptr<sao_plugins_registry>& owned,
                                      sao_plugins_registry** out,
                                      sao_status_t failure_status) noexcept {
    const int32_t cleanup_status = rollbackRegistry(*owned->body);
    if (cleanup_status == SAO_OK)
        return failure_status;
    *out = owned.release();
    return cleanup_status;
}

std::string dependencyId(std::string requirement) {
    if (requirement.find(':') != std::string::npos)
        return {};
    const auto stop = requirement.find_first_of("<>=!~; ");
    if (stop != std::string::npos)
        requirement.resize(stop);
    return requirement;
}

std::vector<bool> runtimeDeferred(const sao_plugins_registry_body& body) {
    std::vector<bool> blocked(body.handles.size(), false);
    const bool python_ready = body.python_runtime_status == SAO_PLUGINS_PYTHON_RUNTIME_READY;
    for (size_t index = 0; index < body.manifests.size(); ++index) {
        const auto& manifest = body.manifests[index];
        blocked[index] = !python_ready && manifest.native_entry.empty() &&
                         manifest.language == loader::engine_kind::python;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t index = 0; index < body.manifests.size(); ++index) {
            if (blocked[index])
                continue;
            for (const auto& requirement : body.manifests[index].requires_list) {
                const auto dependency = dependencyId(requirement);
                const auto found = std::find_if(body.manifests.begin(), body.manifests.end(),
                                                [&dependency](const auto& candidate) {
                                                    return candidate.plugin_id == dependency;
                                                });
                if (found != body.manifests.end() &&
                    blocked[static_cast<size_t>(std::distance(body.manifests.begin(), found))]) {
                    blocked[index] = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    std::vector<bool> deferred(body.handles.size(), false);
    for (size_t index = 0; index < deferred.size(); ++index)
        deferred[index] = body.autostart[index] && blocked[index];
    return deferred;
}

} // namespace

extern "C" size_t sao_launcher_test_platform_timer_count() noexcept {
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    return g_platform_timer_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

extern "C" size_t sao_launcher_test_platform_timer_worker_count() noexcept {
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    return g_platform_timer_worker_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

extern "C" uint64_t sao_launcher_test_platform_timer_unregister_attempt_count() noexcept {
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    return g_platform_timer_unregister_attempt_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

extern "C" void sao_launcher_test_fire_timer_during_register(bool enabled) noexcept {
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    g_test_fire_timer_during_register.store(enabled, std::memory_order_relaxed);
#else
    (void)enabled;
#endif
}

extern "C" void sao_launcher_test_fail_next_timer_unregister(bool enabled) noexcept {
#if defined(SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK)
    g_test_fail_next_timer_unregister.store(enabled ? SAO_SDK_ERR_INTERNAL : SAO_SDK_OK,
                                            std::memory_order_relaxed);
#else
    (void)enabled;
#endif
}

extern "C" sao_status_t sao_plugins_discover(sao_platform_ctx*, sao_plugins_registry** out) {
    if (out == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<sao_plugins_registry> owned;
    try {
        const auto configuration = sao::launcher::launcherProviderConfigurationSnapshot().plugins;
        if (!configuration.enabled ||
            (configuration.roots.empty() && configuration.user_roots.empty() &&
             configuration.manifests.empty())) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        if (!configuredPathsExist(configuration)) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        owned = std::make_unique<sao_plugins_registry>();
        owned->body = std::make_shared<sao_plugins_registry_body>();
        owned->body->registry = loader::sao_plugins_registry_instance();
        const int32_t provider_status = registerProductionProviders(*owned->body);
        if (provider_status != SAO_OK) {
            return rollbackDiscoveryFailure(owned, out, provider_status);
        }
        const int32_t adapter_status = registerHostAdapters(*owned->body, configuration);
        if (adapter_status != SAO_OK) {
            return rollbackDiscoveryFailure(owned, out, adapter_status);
        }
        std::unordered_set<std::string> ids;

        loader::scan_config scan;
        scan.builtin_roots = configuration.roots;
        scan.user_roots = configuration.user_roots;
        scan.enable_workspace_walkup = configuration.workspace_walkup;
        scan.max_depth = configuration.max_depth;
        loader::scanned_plugin* discovered = nullptr;
        size_t count = 0;
        if (loader::sao_plugins_scanner_discover(&scan, &discovered, &count) != SAO_OK) {
            return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
        }
        for (size_t index = 0; index < count; ++index) {
            if (!addManifest(*owned->body, discovered[index].manifest, ids)) {
                loader::sao_plugins_scanner_free(discovered, count);
                return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
            }
        }
        loader::sao_plugins_scanner_free(discovered, count);

        for (const auto& manifest_path : configuration.manifests) {
            loader::plugin_manifest manifest;
            if (!loadConfiguredManifest(manifest_path, manifest) ||
                !addManifest(*owned->body, manifest, ids)) {
                return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
            }
        }
        *out = owned.release();
        return SAO_STATUS_OK;
    } catch (...) {
        if (owned != nullptr) {
            return rollbackDiscoveryFailure(owned, out, SAO_STATUS_PLUGIN_LOAD_FAIL);
        }
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
}

extern "C" sao_status_t sao_plugins_activate_autostart(sao_plugins_registry* registry) {
    RegistryLease lease;
    const sao_status_t lease_status = lease.acquire(registry);
    if (lease_status != SAO_STATUS_OK)
        return lease_status;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        auto& body = lease.body();
        std::lock_guard operation_lock(body.operation_mutex);
        if (body.handles.empty())
            return SAO_STATUS_OK;
        std::vector<loader::plugin_handle_t> sorted(body.handles.size());
        if (loader::sao_plugins_lifecycle_topo_sort(body.handles.data(), body.handles.size(),
                                                    sorted.data()) != SAO_OK) {
            return SAO_STATUS_PLUGIN_LOAD_FAIL;
        }
        const auto deferred = runtimeDeferred(body);
        body.deferred_count =
            static_cast<uint32_t>(std::count(deferred.begin(), deferred.end(), true));
        body.python_launch_strategy = body.python_runtime_status == SAO_PLUGINS_PYTHON_RUNTIME_READY
                                          ? SAO_PLUGINS_PYTHON_LAUNCH_IN_PROCESS
                                          : SAO_PLUGINS_PYTHON_LAUNCH_DEFER_DEGRADED;
        for (const auto handle : sorted) {
            const auto found = std::find(body.handles.begin(), body.handles.end(), handle);
            const auto index = static_cast<size_t>(std::distance(body.handles.begin(), found));
            if (found != body.handles.end() && body.autostart[index] && !deferred[index] &&
                loader::sao_plugins_lifecycle_load(handle) != SAO_OK) {
                return SAO_STATUS_PLUGIN_LOAD_FAIL;
            }
        }
        body.last_operation_status = SAO_STATUS_OK;
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_PLUGIN_LOAD_FAIL;
    }
}

extern "C" sao_status_t sao_plugins_reload_all(sao_plugins_registry* registry) {
    RegistryLease lease;
    const sao_status_t lease_status = lease.acquire(registry);
    if (lease_status != SAO_STATUS_OK)
        return lease_status;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        auto& body = lease.body();
        std::lock_guard operation_lock(body.operation_mutex);
        if (body.operational_status == SAO_PLUGINS_OPERATIONAL_DEGRADED) {
            return SAO_STATUS_INTERNAL;
        }
        if (body.handles.empty()) {
            body.last_operation_status = SAO_STATUS_OK;
            body.last_rollback_status = SAO_STATUS_OK;
            body.rollback_attempted = false;
            body.rollback_succeeded = false;
            return SAO_STATUS_OK;
        }
        std::vector<loader::plugin_handle_t> sorted(body.handles.size());
        const int32_t sort_status = loader::sao_plugins_lifecycle_topo_sort(
            body.handles.data(), body.handles.size(), sorted.data());
        if (sort_status != SAO_OK) {
            body.last_operation_status = sort_status;
            body.last_rollback_status = SAO_STATUS_OK;
            body.rollback_attempted = false;
            body.rollback_succeeded = false;
            return sort_status;
        }

        std::vector<ReloadTarget> reload_targets;
        reload_targets.reserve(sorted.size());
        for (const auto handle : sorted) {
            const auto found = std::find(body.handles.begin(), body.handles.end(), handle);
            if (found == body.handles.end())
                return SAO_STATUS_PLUGIN_LOAD_FAIL;
            const auto lifecycle_state = loader::sao_plugins_lifecycle_state(handle);
            if (isLoadedState(lifecycle_state)) {
                reload_targets.push_back({handle, lifecycle_state});
            }
        }

        const auto fail_reload = [&](sao_status_t original_status,
                                     const std::unordered_set<loader::plugin_handle_t>& affected) {
            const sao_status_t restore_status = restoreReloadTargets(reload_targets, affected);
            body.rollback_attempted = true;
            body.rollback_succeeded = restore_status == SAO_STATUS_OK;
            body.last_rollback_status = restore_status;
            if (restore_status != SAO_STATUS_OK) {
                body.operational_status = SAO_PLUGINS_OPERATIONAL_DEGRADED;
                body.last_operation_status = original_status;
                return static_cast<sao_status_t>(SAO_STATUS_INTERNAL);
            }
            body.operational_status = SAO_PLUGINS_OPERATIONAL_READY;
            body.last_operation_status = original_status;
            return original_status;
        };

        std::unordered_set<loader::plugin_handle_t> affected;
        affected.reserve(reload_targets.size());
        for (auto iterator = reload_targets.rbegin(); iterator != reload_targets.rend();
             ++iterator) {
            const int32_t status = loader::sao_plugins_lifecycle_unload(iterator->handle);
            affected.insert(iterator->handle);
            if (status != SAO_OK) {
                return fail_reload(status, affected);
            }
        }
        for (const auto& target : reload_targets) {
            affected.insert(target.handle);
            int32_t status = loader::sao_plugins_lifecycle_load(target.handle);
            if (status == SAO_OK) {
                const auto current = loader::sao_plugins_lifecycle_state(target.handle);
                if (target.original_state == loader::lifecycle_state::loaded_active &&
                    current != loader::lifecycle_state::loaded_active) {
                    status = loader::sao_plugins_lifecycle_enable(target.handle);
                } else if (target.original_state == loader::lifecycle_state::loaded_disabled &&
                           current != loader::lifecycle_state::loaded_disabled) {
                    status = loader::sao_plugins_lifecycle_disable(target.handle);
                }
            }
            if (status != SAO_OK) {
                return fail_reload(status, affected);
            }
        }
        body.operational_status = SAO_PLUGINS_OPERATIONAL_READY;
        body.last_operation_status = SAO_STATUS_OK;
        body.last_rollback_status = SAO_STATUS_OK;
        body.rollback_attempted = false;
        body.rollback_succeeded = false;
        return SAO_STATUS_OK;
    } catch (...) {
        try {
            auto& body = lease.body();
            std::lock_guard operation_lock(body.operation_mutex);
            body.operational_status = SAO_PLUGINS_OPERATIONAL_DEGRADED;
            body.last_operation_status = SAO_STATUS_INTERNAL;
            body.last_rollback_status = SAO_STATUS_OK;
            body.rollback_attempted = false;
            body.rollback_succeeded = false;
        } catch (...) {
        }
        return SAO_STATUS_INTERNAL;
    }
}

extern "C" sao_status_t sao_plugins_status_snapshot(sao_plugins_registry* registry,
                                                    sao_plugins_status_snapshot_t* out_status) {
    if (out_status == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    const std::size_t caller_size = out_status->struct_size;
    if (caller_size < sizeof(out_status->struct_size))
        return SAO_STATUS_INVALID_ARGUMENT;
    RegistryLease lease;
    const sao_status_t lease_status = lease.acquire(registry);
    if (lease_status != SAO_STATUS_OK)
        return lease_status;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        auto& body = lease.body();
        std::lock_guard operation_lock(body.operation_mutex);
        sao_plugins_status_snapshot_t candidate{};
        const std::size_t copy_size = (std::min)(caller_size, sizeof(candidate));
        candidate.struct_size = static_cast<std::uint32_t>(copy_size);
        candidate.python_runtime_status = body.python_runtime_status;
        candidate.python_launch_strategy = body.python_launch_strategy;
        candidate.operational_status = body.operational_status;
        candidate.last_operation_status = body.last_operation_status;
        candidate.last_rollback_status = body.last_rollback_status;
        candidate.rollback_attempted = body.rollback_attempted ? 1U : 0U;
        candidate.rollback_succeeded = body.rollback_succeeded ? 1U : 0U;
        candidate.discovered_count = static_cast<uint32_t>(body.handles.size());
        candidate.deferred_count = body.deferred_count;
        candidate.active_call_count = lease.active_calls();
        for (const auto handle : body.handles) {
            const auto state = loader::sao_plugins_lifecycle_state(handle);
            if (state == loader::lifecycle_state::loaded_active ||
                state == loader::lifecycle_state::loaded_disabled) {
                ++candidate.loaded_count;
            }
            if (state == loader::lifecycle_state::loaded_active) {
                ++candidate.enabled_count;
            }
        }
        std::memcpy(out_status, &candidate, copy_size);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_INTERNAL;
    }
}

extern "C" sao_status_t sao_plugins_shutdown(sao_plugins_registry* registry) {
    if (registry == nullptr)
        return SAO_STATUS_INVALID_ARGUMENT;
    ProviderOperationGuard operation(registry);
    if (!operation.acquired())
        return loader::SAO_PLUGINS_ERR_BUSY;
    try {
        std::shared_ptr<sao_plugins_registry_body> body;
        {
            std::unique_lock lock(registry->mutex);
            if (registry->retired)
                return SAO_STATUS_OK;
            if (registry->retiring)
                return loader::SAO_PLUGINS_ERR_BUSY;
            registry->retiring = true;
            body = registry->body;
            registry->idle.wait(lock, [registry] { return registry->active_calls == 0; });
        }
        if (!body) {
            std::lock_guard lock(registry->mutex);
            registry->retiring = false;
            registry->retired = true;
            return SAO_STATUS_OK;
        }

        std::lock_guard operation_lock(body->operation_mutex);
        const int32_t status = rollbackRegistry(*body);
        if (status != SAO_OK) {
            body->last_operation_status = status;
            std::lock_guard lock(registry->mutex);
            registry->retiring = false;
            registry->idle.notify_all();
            return status;
        }
        body->operational_status = SAO_PLUGINS_OPERATIONAL_SHUTDOWN;
        body->last_operation_status = SAO_STATUS_OK;
        {
            std::lock_guard lock(registry->mutex);
            registry->body.reset();
            registry->retiring = false;
            registry->retired = true;
        }
        registry->idle.notify_all();
        return SAO_STATUS_OK;
    } catch (...) {
        try {
            std::lock_guard lock(registry->mutex);
            registry->retiring = false;
            registry->idle.notify_all();
        } catch (...) {
        }
        return SAO_STATUS_INTERNAL;
    }
}