// SAO Auto — SDK context lifecycle and shared runtime.
//
// Wires:
//   * sao_sdk_context_create / _destroy — allocate a per-plugin state
//     bag, populate the vtables, hand back a SaoSdkContext.
//   * sao_sdk_context_get_plugin_id / _get_base_dir — trivial accessors.
//   * SharedRuntime::instance() — process-wide compositor / event bus /
//     input router; created lazily on the first context_create.
//   * fire_render_hook_test — used by the demo test driver to run the
//     UI clock across every live context without needing a real
//     compositor tick.
//
// Only lightweight compat state lives here — the vtable factories that
// forward into the platform live in sdk_ui_wire.cpp / sdk_event_wire.cpp
// / sdk_hotkey_wire.cpp.
//
// PLATFORM DOES NOT IMPORT PLUGINS — this file speaks only to
// sao::core / sao::engine / sao::ui.  A plugin sees SaoSdkContext and
// the wire helpers; nothing here.

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include "sdk_callback_barrier.h"
#include "sao/sdk/sao_sdk_platform_internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sao/engine/render_hook.h"

namespace sao_sdk_internal {

// ─── Shared runtime ──────────────────────────────────────────────────

SharedRuntime& SharedRuntime::instance() {
    static SharedRuntime rt;
    return rt;
}

namespace {

sao_sdk_status_t map_runtime_status(sao_status_t status) noexcept {
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
    case SAO_UI_STATUS_ERR_BUSY:
        return SAO_SDK_ERR_BUSY;
    default:
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t ensure_base_runtime_locked(SharedRuntime& runtime) {
    bool created_event_bus = false;
    if (runtime.event_bus == nullptr) {
        const sao_status_t status = sao_engine_event_bus_create_priority(&runtime.event_bus);
        if (status != SAO_STATUS_OK)
            return map_runtime_status(status);
        created_event_bus = true;
    }
    if (runtime.render_registry == nullptr) {
        const sao_status_t status =
            sao_engine_render_hook_registry_create(&runtime.render_registry);
        if (status != SAO_STATUS_OK) {
            if (created_event_bus) {
                sao_engine_event_bus_destroy(runtime.event_bus);
                runtime.event_bus = nullptr;
            }
            return map_runtime_status(status);
        }
    }
    return SAO_SDK_OK;
}

sao_sdk_status_t start_runtime_locked(SharedRuntime& runtime) {
    const sao_sdk_status_t base_status = ensure_base_runtime_locked(runtime);
    if (base_status != SAO_SDK_OK)
        return base_status;
    bool created_compositor = false;
    if (runtime.compositor == nullptr) {
        SaoCompositorConfig config{};
        config.enable_temporal_union = true;
        config.enable_rgn_cache = true;
        const sao_status_t status =
            sao_ui_compositor_create(nullptr, &config, &runtime.compositor);
        if (status != SAO_STATUS_OK)
            return map_runtime_status(status);
        runtime.owns_compositor = true;
        runtime.compositor_bound = false;
        runtime.compositor_owner_thread = std::this_thread::get_id();
        created_compositor = true;
    }
    if (runtime.input_router == nullptr) {
        const sao_status_t status =
            sao_ui_input_router_deep_create(runtime.compositor, &runtime.input_router);
        if (status != SAO_STATUS_OK) {
            if (created_compositor) {
                const sao_status_t destroy_status =
                    sao_ui_compositor_try_destroy(runtime.compositor);
                if (destroy_status == SAO_STATUS_OK) {
                    runtime.compositor = nullptr;
                    runtime.owns_compositor = false;
                    runtime.compositor_owner_thread = {};
                }
            }
            return map_runtime_status(status);
        }
    }
    return SAO_SDK_OK;
}

} // namespace

sao_sdk_status_t SharedRuntime::ensure_started() {
    try {
        std::lock_guard<std::mutex> guard(mu);
        return start_runtime_locked(*this);
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t SharedRuntime::acquire_context() {
    try {
        std::lock_guard<std::mutex> guard(mu);
        const sao_sdk_status_t status = start_runtime_locked(*this);
        if (status != SAO_SDK_OK)
            return status;
        ++active_contexts;
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

void SharedRuntime::release_context() noexcept {
    try {
        std::lock_guard<std::mutex> guard(mu);
        if (active_contexts > 0)
            --active_contexts;
    } catch (...) {
    }
}

sao_sdk_status_t SharedRuntime::bind_compositor(sao_ui_compositor_handle_t replacement) {
    if (replacement == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const sao_status_t replacement_owner = sao_ui_compositor_require_owner_thread(replacement);
    if (replacement_owner != SAO_STATUS_OK)
        return map_runtime_status(replacement_owner);
    try {
        std::lock_guard<std::mutex> guard(mu);
        if (active_contexts != 0)
            return SAO_SDK_ERR_BUSY;
        if (compositor_bound)
            return SAO_SDK_ERR_ALREADY_EXISTS;

        const sao_sdk_status_t base_status = ensure_base_runtime_locked(*this);
        if (base_status != SAO_SDK_OK)
            return base_status;
        if (compositor == replacement)
            return SAO_SDK_ERR_ALREADY_EXISTS;
        if (owns_compositor &&
            sao_ui_compositor_require_owner_thread(compositor) != SAO_STATUS_OK) {
            return SAO_SDK_ERR_ACCESS_DENIED;
        }

        if (owns_compositor && compositor != nullptr) {
            const sao_status_t preflight_status =
                sao_ui_compositor_destroy_preflight(compositor);
            if (preflight_status != SAO_STATUS_OK)
                return map_runtime_status(preflight_status);
        }
        auto* const previous_compositor = compositor;
        auto* const previous_router = input_router;
        sao_ui_input_router_deep_handle_t retained_router = previous_router;
        bool created_router = false;
        if (previous_router != nullptr) {
            const sao_status_t rebind_status = sao_ui_input_router_deep_rebind_compositor(
                previous_router, previous_compositor, replacement);
            if (rebind_status != SAO_STATUS_OK)
                return map_runtime_status(rebind_status);
        } else {
            const sao_status_t create_status =
                sao_ui_input_router_deep_create(replacement, &retained_router);
            if (create_status != SAO_STATUS_OK)
                return map_runtime_status(create_status);
            created_router = true;
        }
        if (owns_compositor && previous_compositor != nullptr) {
            const sao_status_t destroy_status =
                sao_ui_compositor_try_destroy(previous_compositor);
            if (destroy_status != SAO_STATUS_OK) {
                if (created_router) {
                    (void)sao_ui_input_router_deep_try_destroy(retained_router);
                } else {
                    const sao_status_t rollback_status =
                        sao_ui_input_router_deep_rebind_compositor(
                            retained_router, replacement, previous_compositor);
                    if (rollback_status != SAO_STATUS_OK)
                        return map_runtime_status(rollback_status);
                }
                return map_runtime_status(destroy_status);
            }
        }
        compositor = replacement;
        input_router = retained_router;
        owns_compositor = false;
        compositor_bound = true;
        compositor_owner_thread = std::this_thread::get_id();
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t SharedRuntime::unbind_compositor() {
    try {
        std::lock_guard<std::mutex> guard(mu);
        if (!compositor_bound || compositor == nullptr)
            return SAO_SDK_ERR_NOT_INITIALIZED;
        if (std::this_thread::get_id() != compositor_owner_thread)
            return SAO_SDK_ERR_ACCESS_DENIED;
        if (active_contexts != 0)
            return SAO_SDK_ERR_BUSY;
        const sao_status_t router_status =
            sao_ui_input_router_deep_try_destroy(input_router);
        if (router_status != SAO_STATUS_OK)
            return map_runtime_status(router_status);
        input_router = nullptr;
        compositor = nullptr;
        owns_compositor = false;
        compositor_bound = false;
        compositor_owner_thread = {};
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

sao_sdk_status_t
SharedRuntime::get_bound_compositor(sao_ui_compositor_handle_t* out_compositor) {
    if (out_compositor == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_compositor = nullptr;
    try {
        std::lock_guard<std::mutex> guard(mu);
        if (!compositor_bound || compositor == nullptr)
            return SAO_SDK_ERR_NOT_INITIALIZED;
        *out_compositor = compositor;
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

// ─── Context registry (process-wide) ─────────────────────────────────

namespace {
std::mutex g_ctx_registry_mu;
std::condition_variable g_ctx_registry_idle;
std::unordered_set<ContextState*> g_ctx_registry;
std::unordered_set<ContextState*> g_destroy_quarantine;
std::unordered_map<ContextState*, size_t> g_ctx_snapshot_leases;
std::unordered_map<const SaoSdkContext*, ContextState*> g_ctx_by_public_context;

std::mutex g_api_test_pause_mutex;
std::condition_variable g_api_test_pause_changed;
ContextApiTestPoint g_api_test_pause_point{};
bool g_api_test_pause_armed = false;
bool g_api_test_pause_entered = false;
bool g_api_test_pause_released = false;
} // namespace

void register_context(ContextState* state) {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    state->callback_gate->state = state;
    g_ctx_registry.insert(state);
    g_ctx_snapshot_leases.try_emplace(state, 0);
    g_ctx_by_public_context[state->bound_public_ctx] = state;
}

void unregister_context(ContextState* state) {
    {
        std::unique_lock<std::mutex> guard(g_ctx_registry_mu);
        state->api_accepting = false;
        g_ctx_registry.erase(state);
        g_destroy_quarantine.erase(state);
        g_ctx_by_public_context.erase(state->bound_public_ctx);
        g_ctx_registry_idle.wait(guard, [state] {
            const auto found = g_ctx_snapshot_leases.find(state);
            return state->active_api_calls == 0 &&
                   (found == g_ctx_snapshot_leases.end() || found->second == 0);
        });
        g_ctx_snapshot_leases.erase(state);
    }
    {
        std::lock_guard<std::mutex> callback_lock(state->callback_gate->mutex);
        state->callback_gate->state = nullptr;
        state->callback_gate->accepting = false;
    }
}

ContextApiLease::ContextApiLease(const SaoSdkContext* context) noexcept {
    if (context == nullptr) {
        status_ = SAO_SDK_ERR_INVALID_ARGUMENT;
        return;
    }
    try {
        std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
        const auto found = g_ctx_by_public_context.find(context);
        if (found == g_ctx_by_public_context.end()) {
            status_ = SAO_SDK_ERR_HANDLE_INVALID;
            return;
        }
        auto* state = found->second;
        if (g_context_api_owner == state) {
            if (state->destroy_quarantined.load(std::memory_order_acquire)) {
                status_ = SAO_SDK_ERR_BUSY;
                return;
            }
            state_ = state;
            previous_owner_ = g_context_api_owner;
            status_ = SAO_SDK_OK;
            return;
        }
        if (!state->api_accepting || state->destroying.load(std::memory_order_acquire)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        ++state->active_api_calls;
        state_ = state;
        previous_owner_ = g_context_api_owner;
        g_context_api_owner = state;
        owns_lease_ = true;
        status_ = SAO_SDK_OK;
    } catch (...) {
        state_ = nullptr;
        status_ = SAO_SDK_ERR_INTERNAL;
    }
}

ContextApiLease::ContextApiLease(ContextState* state) noexcept {
    if (state == nullptr) {
        status_ = SAO_SDK_ERR_HANDLE_INVALID;
        return;
    }
    if (g_context_api_owner == state) {
        if (state->destroy_quarantined.load(std::memory_order_acquire)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        state_ = state;
        previous_owner_ = g_context_api_owner;
        status_ = SAO_SDK_OK;
        return;
    }
    try {
        std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
        if (!g_ctx_registry.contains(state)) {
            status_ = SAO_SDK_ERR_HANDLE_INVALID;
            return;
        }
        if (!state->api_accepting || state->destroying.load(std::memory_order_acquire)) {
            status_ = SAO_SDK_ERR_BUSY;
            return;
        }
        ++state->active_api_calls;
        state_ = state;
        previous_owner_ = g_context_api_owner;
        g_context_api_owner = state;
        owns_lease_ = true;
        status_ = SAO_SDK_OK;
    } catch (...) {
        state_ = nullptr;
        status_ = SAO_SDK_ERR_INTERNAL;
    }
}

ContextApiLease::~ContextApiLease() {
    if (state_ == nullptr)
        return;
    g_context_api_owner = previous_owner_;
    if (!owns_lease_)
        return;
    try {
        std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
        if (state_->active_api_calls != 0)
            --state_->active_api_calls;
        if (state_->active_api_calls == 0)
            g_ctx_registry_idle.notify_all();
    } catch (...) {
    }
}

void quarantine_context(ContextState* state) {
    if (state == nullptr)
        return;
    try {
        {
            std::lock_guard<std::mutex> callback_lock(state->callback_gate->mutex);
            state->callback_gate->accepting = false;
        }
        {
            std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
            state->api_accepting = false;
            state->destroying.store(true, std::memory_order_release);
            state->destroy_quarantined.store(true, std::memory_order_release);
            g_ctx_registry.insert(state);
            g_destroy_quarantine.insert(state);
            g_ctx_snapshot_leases.try_emplace(state, 0);
            g_ctx_by_public_context[state->bound_public_ctx] = state;
        }
    } catch (...) {
    }
}

void quarantine_public_context(const SaoSdkContext* context) {
    if (context == nullptr)
        return;
    try {
        ContextState* state = nullptr;
        {
            std::lock_guard<std::mutex> registry_lock(g_ctx_registry_mu);
            const auto found = g_ctx_by_public_context.find(context);
            if (found == g_ctx_by_public_context.end())
                return;
            state = found->second;
            state->destroying.store(true, std::memory_order_release);
            state->api_accepting = false;
            state->destroy_quarantined.store(true, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> callback_lock(state->callback_gate->mutex);
            state->callback_gate->accepting = false;
        }
        std::lock_guard<std::mutex> registry_lock(g_ctx_registry_mu);
        g_destroy_quarantine.insert(state);
    } catch (...) {
    }
}

void unquarantine_context(ContextState* state) {
    if (state == nullptr)
        return;
    {
        std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
        state->destroy_quarantined.store(false, std::memory_order_release);
        state->api_accepting = !state->destroying.load(std::memory_order_acquire);
        g_destroy_quarantine.erase(state);
    }
    std::lock_guard<std::mutex> callback_lock(state->callback_gate->mutex);
    state->callback_gate->accepting = !state->destroying.load(std::memory_order_acquire);
}

size_t live_context_count() {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    return g_ctx_registry.size();
}

// Snapshot the alive contexts (copy).  Called from
// fire_render_hook_test — we drop the lock before firing user code
// (callbacks must never run while we hold the registry mutex).
static std::vector<ContextState*> snapshot_contexts() {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    std::vector<ContextState*> contexts(g_ctx_registry.begin(), g_ctx_registry.end());
    for (auto* state : contexts)
        ++g_ctx_snapshot_leases[state];
    return contexts;
}

void release_context_snapshot(ContextState* state) noexcept {
    std::lock_guard<std::mutex> guard(g_ctx_registry_mu);
    const auto found = g_ctx_snapshot_leases.find(state);
    if (found == g_ctx_snapshot_leases.end() || found->second == 0)
        return;
    --found->second;
    if (found->second == 0)
        g_ctx_registry_idle.notify_all();
}

sao_sdk_status_t begin_context_shutdown(const SaoSdkContext* context, ContextState** out_state,
                                        std::unique_lock<std::mutex>* out_destroy_lock) {
    if (out_state == nullptr || out_destroy_lock == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_state = nullptr;
    *out_destroy_lock = {};
    if (context == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;

    ContextState* state = nullptr;
    std::unique_lock<std::mutex> destroy_lock;
    {
        std::unique_lock<std::mutex> registry_lock(g_ctx_registry_mu);
        const auto found = g_ctx_by_public_context.find(context);
        if (found == g_ctx_by_public_context.end())
            return SAO_SDK_ERR_HANDLE_INVALID;
        state = found->second;
        if (context_api_reentered(state) || plugin_callback_reentered(state) ||
            provider_callback_reentered(state) || memory_callback_reentered(state) ||
            net_callback_reentered(state) || gpu_callback_reentered(state)) {
            return SAO_SDK_ERR_BUSY;
        }
        destroy_lock = std::unique_lock<std::mutex>(state->destroy_mutex, std::try_to_lock);
        if (!destroy_lock.owns_lock())
            return SAO_SDK_ERR_BUSY;
        if (state->destroying.load(std::memory_order_acquire) &&
            !state->destroy_quarantined.load(std::memory_order_acquire))
            return SAO_SDK_ERR_BUSY;
        state->destroying.store(true, std::memory_order_release);
        state->api_accepting = false;
        g_ctx_registry_idle.notify_all();
        g_context_destroy_owner = state;
        g_ctx_registry_idle.wait(registry_lock,
                                 [state] { return state->active_api_calls == 0; });
    }

    {
        std::unique_lock<std::mutex> lock(state->callback_gate->mutex);
        state->callback_gate->accepting = false;
        state->callback_gate->idle.wait(lock, [state] { return state->callback_gate->active == 0; });
    }
    *out_state = state;
    *out_destroy_lock = std::move(destroy_lock);
    return SAO_SDK_OK;
}

void cancel_context_shutdown(ContextState* state) noexcept {
    if (state == nullptr)
        return;
    if (g_context_destroy_owner == state)
        g_context_destroy_owner = nullptr;
    const bool quarantined = state->destroy_quarantined.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> registry_lock(g_ctx_registry_mu);
        state->destroying.store(false, std::memory_order_release);
        state->api_accepting = !quarantined;
    }
    std::lock_guard<std::mutex> callback_lock(state->callback_gate->mutex);
    state->callback_gate->accepting = !quarantined;
}

void pause_context_api_test_point(ContextApiTestPoint point) {
#if !defined(SAO_SDK_TESTING)
    (void)point;
    return;
#else
    std::unique_lock<std::mutex> lock(g_api_test_pause_mutex);
    if (!g_api_test_pause_armed || g_api_test_pause_point != point)
        return;
    g_api_test_pause_entered = true;
    g_api_test_pause_changed.notify_all();
    g_api_test_pause_changed.wait(lock, [] { return g_api_test_pause_released; });
    g_api_test_pause_armed = false;
    g_api_test_pause_entered = false;
    g_api_test_pause_released = false;
#endif
}

void arm_context_api_test_pause(ContextApiTestPoint point) {
    std::lock_guard<std::mutex> lock(g_api_test_pause_mutex);
    g_api_test_pause_point = point;
    g_api_test_pause_armed = true;
    g_api_test_pause_entered = false;
    g_api_test_pause_released = false;
}

bool wait_for_context_api_test_pause(ContextApiTestPoint point) {
    std::unique_lock<std::mutex> lock(g_api_test_pause_mutex);
    return g_api_test_pause_changed.wait_for(lock, std::chrono::seconds(5), [point] {
        return g_api_test_pause_armed && g_api_test_pause_entered &&
               g_api_test_pause_point == point;
    });
}

void resume_context_api_test_pause(ContextApiTestPoint point) {
    std::lock_guard<std::mutex> lock(g_api_test_pause_mutex);
    if (!g_api_test_pause_armed || g_api_test_pause_point != point)
        return;
    g_api_test_pause_released = true;
    g_api_test_pause_changed.notify_all();
}

bool wait_for_context_shutdown(const SaoSdkContext* context) {
    std::unique_lock<std::mutex> lock(g_ctx_registry_mu);
    return g_ctx_registry_idle.wait_for(lock, std::chrono::seconds(5), [context] {
        const auto found = g_ctx_by_public_context.find(context);
        return found == g_ctx_by_public_context.end() || !found->second->api_accepting;
    });
}

void fire_render_hook_test(int32_t hook_point, const SaoSdkRenderHookPayload& payload) {
    auto ctxs = snapshot_contexts();
    for (auto* state : ctxs) {
        struct SnapshotGuard {
            ContextState* state;
            ~SnapshotGuard() {
                release_context_snapshot(state);
            }
        } snapshot_guard{state};
        // Snapshot the hook list under the per-context lock, then fire
        // outside so callbacks that re-enter the SDK don't deadlock.
        std::vector<RenderHookEntry> hooks_copy;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            hooks_copy = state->render_hooks;
        }
        for (const auto& hook : hooks_copy) {
            if (hook.hook_point == hook_point && hook.callback != nullptr) {
                PluginCallbackLease callback_lease(state);
                if (callback_lease) {
                    (void)invoke_callback_barrier([&hook, &payload] {
                        return hook.callback(hook.hook_point, &payload, hook.user_data);
                    });
                }
            }
        }
    }
}

// ─── Capability sub-tables ──────────────────────────────────────────

namespace {

bool add_signed_offset(uint64_t base, int32_t offset, uint64_t* out_value) {
    if (offset >= 0) {
        const auto positive = static_cast<uint64_t>(offset);
        if (base > UINT64_MAX - positive)
            return false;
        *out_value = base + positive;
        return true;
    }
    const auto magnitude = static_cast<uint64_t>(-static_cast<int64_t>(offset));
    if (base < magnitude)
        return false;
    *out_value = base - magnitude;
    return true;
}

bool equal_module_name(const char* left, const char* right) {
    if (left == nullptr || right == nullptr)
        return false;
    while (*left != '\0' && *right != '\0') {
        auto lhs = static_cast<unsigned char>(*left++);
        auto rhs = static_cast<unsigned char>(*right++);
        if (lhs >= 'A' && lhs <= 'Z')
            lhs = static_cast<unsigned char>(lhs - 'A' + 'a');
        if (rhs >= 'A' && rhs <= 'Z')
            rhs = static_cast<unsigned char>(rhs - 'A' + 'a');
        if (lhs != rhs)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

sao_sdk_status_t SAO_SDK_CALL mem_read(void* ctx_impl, uint64_t address, void* out_buffer,
                                       size_t buffer_size, size_t* out_bytes_read) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return memory_read(cast_ctx(ctx_impl), address, out_buffer, buffer_size, out_bytes_read);
}

sao_sdk_status_t SAO_SDK_CALL mem_read_u32(void* ctx_impl, uint64_t address, uint32_t* out_value) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_value = 0;
    uint32_t value = 0;
    size_t bytes_read = 0;
    const auto status =
        memory_read(cast_ctx(ctx_impl), address, &value, sizeof(value), &bytes_read);
    if (status == SAO_SDK_OK && bytes_read == sizeof(value)) {
        *out_value = value;
        return SAO_SDK_OK;
    }
    return status == SAO_SDK_OK ? SAO_SDK_ERR_READ_FAULT : status;
}

sao_sdk_status_t SAO_SDK_CALL mem_read_u64(void* ctx_impl, uint64_t address, uint64_t* out_value) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_value = 0;
    uint64_t value = 0;
    size_t bytes_read = 0;
    const auto status =
        memory_read(cast_ctx(ctx_impl), address, &value, sizeof(value), &bytes_read);
    if (status == SAO_SDK_OK && bytes_read == sizeof(value)) {
        *out_value = value;
        return SAO_SDK_OK;
    }
    return status == SAO_SDK_OK ? SAO_SDK_ERR_READ_FAULT : status;
}

sao_sdk_status_t SAO_SDK_CALL mem_read_ptr_chain(void* ctx_impl, uint64_t base_address,
                                                 const int32_t* offsets, size_t offset_count,
                                                 uint64_t* out_final_address) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_final_address == nullptr || (offset_count != 0 && offsets == nullptr))
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_final_address = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    const auto attachment_status = memory_attachment_status(state);
    if (attachment_status != SAO_SDK_OK)
        return attachment_status;
    if (base_address == 0)
        return SAO_SDK_ERR_READ_FAULT;
    if (offset_count == 0) {
        *out_final_address = base_address;
        return SAO_SDK_OK;
    }

    uint64_t current_address = base_address;
    for (size_t index = 0; index < offset_count; ++index) {
        uint64_t pointer_value = 0;
        const auto read_status = mem_read_u64(state, current_address, &pointer_value);
        if (read_status != SAO_SDK_OK)
            return read_status;
        if (pointer_value == 0)
            return SAO_SDK_ERR_READ_FAULT;
        uint64_t next_address = 0;
        if (!add_signed_offset(pointer_value, offsets[index], &next_address))
            return SAO_SDK_ERR_READ_FAULT;
        if (next_address == 0)
            return SAO_SDK_ERR_READ_FAULT;
        if (index + 1 == offset_count) {
            *out_final_address = next_address;
            return SAO_SDK_OK;
        }
        current_address = next_address;
    }
    return SAO_SDK_ERR_READ_FAULT;
}

struct ModuleLookup {
    const char* requested_name = nullptr;
    uint64_t base = 0;
    bool found = false;
};

sao_sdk_status_t SAO_SDK_CALL mem_module_base(void* ctx_impl, const char* module_name_utf8,
                                              uint64_t* out_base) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_base == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_base = 0;
    if (module_name_utf8 == nullptr || module_name_utf8[0] == '\0')
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    try {
        ModuleLookup lookup{module_name_utf8};
        size_t module_count = 0;
        auto status = memory_enumerate_modules(cast_ctx(ctx_impl), nullptr, 0,
                                               SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE, &module_count);
        if (status != SAO_SDK_OK && status != SAO_SDK_ERR_BUFFER_TOO_SMALL)
            return status;
        if (module_count > SAO_SDK_MEMORY_MAX_MODULE_COUNT)
            return SAO_SDK_ERR_INTERNAL;
        if (module_count == 0)
            return SAO_SDK_ERR_NOT_FOUND;
        std::vector<SaoSdkMemoryModule> modules(module_count);
        for (auto& module : modules)
            module.struct_size = sizeof(module);
        status = memory_enumerate_modules(cast_ctx(ctx_impl), modules.data(), modules.size(),
                                          SAO_SDK_MEMORY_MODULE_ELEMENT_SIZE, &module_count);
        if (status != SAO_SDK_OK)
            return status;
        if (module_count > modules.size())
            return SAO_SDK_ERR_BUFFER_TOO_SMALL;
        for (size_t index = 0; index < module_count; ++index) {
            const auto& module = modules[index];
            if (std::memchr(module.name_utf8, '\0', sizeof(module.name_utf8)) == nullptr)
                return SAO_SDK_ERR_READ_FAULT;
            if (equal_module_name(module.name_utf8, lookup.requested_name)) {
                lookup.base = module.base_address;
                lookup.found = true;
                break;
            }
        }
        if (!lookup.found)
            return SAO_SDK_ERR_NOT_FOUND;
        *out_base = lookup.base;
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_NOT_INITIALIZED;
    }
}

sao_sdk_status_t SAO_SDK_CALL mem_attach(void* ctx_impl,
                                         const SaoSdkMemoryTargetIdentity* identity) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return memory_attach(cast_ctx(ctx_impl), identity);
}

sao_sdk_status_t SAO_SDK_CALL mem_detach(void* ctx_impl) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return memory_detach(cast_ctx(ctx_impl));
}

sao_sdk_status_t SAO_SDK_CALL mem_enumerate_modules(void* ctx_impl, SaoSdkMemoryModule* out_modules,
                                                    size_t capacity, size_t element_stride,
                                                    size_t* out_count) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return memory_enumerate_modules(cast_ctx(ctx_impl), out_modules, capacity, element_stride,
                                    out_count);
}

sao_sdk_status_t SAO_SDK_CALL mem_read_boundary(void* ctx_impl, uint64_t address,
                                                void* out_buffer, size_t buffer_size,
                                                size_t* out_bytes_read) noexcept {
    return invoke_callback_barrier(
        [&] { return mem_read(ctx_impl, address, out_buffer, buffer_size, out_bytes_read); });
}

sao_sdk_status_t SAO_SDK_CALL mem_read_u32_boundary(void* ctx_impl, uint64_t address,
                                                    uint32_t* out_value) noexcept {
    return invoke_callback_barrier([&] { return mem_read_u32(ctx_impl, address, out_value); });
}

sao_sdk_status_t SAO_SDK_CALL mem_read_u64_boundary(void* ctx_impl, uint64_t address,
                                                    uint64_t* out_value) noexcept {
    return invoke_callback_barrier([&] { return mem_read_u64(ctx_impl, address, out_value); });
}

sao_sdk_status_t SAO_SDK_CALL mem_read_ptr_chain_boundary(
    void* ctx_impl, uint64_t base_address, const int32_t* offsets, size_t offset_count,
    uint64_t* out_final_address) noexcept {
    return invoke_callback_barrier([&] {
        return mem_read_ptr_chain(ctx_impl, base_address, offsets, offset_count,
                                  out_final_address);
    });
}

sao_sdk_status_t SAO_SDK_CALL mem_module_base_boundary(void* ctx_impl, const char* module_name_utf8,
                                                       uint64_t* out_base) noexcept {
    return invoke_callback_barrier(
        [&] { return mem_module_base(ctx_impl, module_name_utf8, out_base); });
}

sao_sdk_status_t SAO_SDK_CALL mem_attach_boundary(
    void* ctx_impl, const SaoSdkMemoryTargetIdentity* identity) noexcept {
    return invoke_callback_barrier([&] { return mem_attach(ctx_impl, identity); });
}

sao_sdk_status_t SAO_SDK_CALL mem_detach_boundary(void* ctx_impl) noexcept {
    return invoke_callback_barrier([&] { return mem_detach(ctx_impl); });
}

sao_sdk_status_t SAO_SDK_CALL mem_enumerate_modules_boundary(
    void* ctx_impl, SaoSdkMemoryModule* out_modules, size_t capacity, size_t element_stride,
    size_t* out_count) noexcept {
    return invoke_callback_barrier([&] {
        return mem_enumerate_modules(ctx_impl, out_modules, capacity, element_stride, out_count);
    });
}

const SaoSdkMemTable kMemTable = {
    mem_read_boundary,
    mem_read_u32_boundary,
    mem_read_u64_boundary,
    mem_read_ptr_chain_boundary,
    mem_module_base_boundary,
    SAO_SDK_MEM_TABLE_ABI_VERSION,
    sizeof(SaoSdkMemTable),
    mem_attach_boundary,
    mem_detach_boundary,
    mem_enumerate_modules_boundary,
};

template <typename T> sao_sdk_status_t cfg_get(void* ctx_impl, const char* key_utf8, T* out_value) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_value != nullptr)
        *out_value = T{};
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || key_utf8[0] == '\0' || out_value == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->config_values.find(key_utf8);
    if (it == state->config_values.end())
        return SAO_SDK_ERR_NOT_FOUND;
    const auto* value = std::get_if<T>(&it->second);
    if (value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_value = *value;
    return SAO_SDK_OK;
}

template <typename T> sao_sdk_status_t cfg_set(void* ctx_impl, const char* key_utf8, T value) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (key_utf8 == nullptr || key_utf8[0] == '\0')
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(state->mu);
    state->config_values[std::string(key_utf8)] = std::move(value);
    return SAO_SDK_OK;
}

sao_sdk_status_t SAO_SDK_CALL cfg_get_bool(void* ctx_impl, const char* key, bool* value) {
    return cfg_get(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_int(void* ctx_impl, const char* key, int64_t* value) {
    return cfg_get(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_double(void* ctx_impl, const char* key, double* value) {
    return cfg_get(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_string(void* ctx_impl, const char* key, char* out_buffer,
                                             size_t buffer_len, size_t* out_bytes_needed) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    if (out_bytes_needed != nullptr)
        *out_bytes_needed = 0;
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (key == nullptr || key[0] == '\0')
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lk(state->mu);
    const auto it = state->config_values.find(key);
    if (it == state->config_values.end())
        return SAO_SDK_ERR_NOT_FOUND;
    const auto* value = std::get_if<std::string>(&it->second);
    if (value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    const size_t required = value->size() + 1;
    if (out_bytes_needed != nullptr)
        *out_bytes_needed = required;
    if (out_buffer == nullptr || buffer_len < required)
        return SAO_SDK_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out_buffer, value->c_str(), required);
    return SAO_SDK_OK;
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_bool(void* ctx_impl, const char* key, bool value) {
    return cfg_set(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_int(void* ctx_impl, const char* key, int64_t value) {
    return cfg_set(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_double(void* ctx_impl, const char* key, double value) {
    return cfg_set(ctx_impl, key, value);
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_string(void* ctx_impl, const char* key, const char* value) {
    if (value == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    return cfg_set(ctx_impl, key, std::string(value));
}

sao_sdk_status_t SAO_SDK_CALL cfg_get_bool_boundary(void* ctx_impl, const char* key, bool* value) noexcept {
    return invoke_callback_barrier([&] { return cfg_get_bool(ctx_impl, key, value); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_int_boundary(void* ctx_impl, const char* key, int64_t* value) noexcept {
    return invoke_callback_barrier([&] { return cfg_get_int(ctx_impl, key, value); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_double_boundary(void* ctx_impl, const char* key, double* value) noexcept {
    return invoke_callback_barrier([&] { return cfg_get_double(ctx_impl, key, value); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_get_string_boundary(void* ctx_impl, const char* key, char* buffer,
                                                      size_t buffer_len, size_t* bytes_needed) noexcept {
    return invoke_callback_barrier(
        [&] { return cfg_get_string(ctx_impl, key, buffer, buffer_len, bytes_needed); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_bool_boundary(void* ctx_impl, const char* key, bool value) noexcept {
    return invoke_callback_barrier([&] { return cfg_set_bool(ctx_impl, key, value); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_int_boundary(void* ctx_impl, const char* key, int64_t value) noexcept {
    return invoke_callback_barrier([&] { return cfg_set_int(ctx_impl, key, value); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_double_boundary(void* ctx_impl, const char* key, double value) noexcept {
    return invoke_callback_barrier([&] { return cfg_set_double(ctx_impl, key, value); });
}
sao_sdk_status_t SAO_SDK_CALL cfg_set_string_boundary(void* ctx_impl, const char* key,
                                                      const char* value) noexcept {
    return invoke_callback_barrier([&] { return cfg_set_string(ctx_impl, key, value); });
}
const SaoSdkConfigTable kConfigTable = {
    cfg_get_bool_boundary, cfg_get_int_boundary, cfg_get_double_boundary, cfg_get_string_boundary,
    cfg_set_bool_boundary, cfg_set_int_boundary, cfg_set_double_boundary, cfg_set_string_boundary,
};

sao_sdk_status_t SAO_SDK_CALL tts_speak(void* ctx_impl, const char* text_utf8, float volume,
                                        float rate) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return provider_tts_speak(cast_ctx(ctx_impl), text_utf8, volume, rate);
}
sao_sdk_status_t SAO_SDK_CALL tts_stop(void* ctx_impl) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    return provider_tts_stop(cast_ctx(ctx_impl));
}
sao_sdk_status_t SAO_SDK_CALL tts_speak_boundary(void* ctx_impl, const char* text_utf8,
                                                  float volume, float rate) noexcept {
    return invoke_callback_barrier([&] { return tts_speak(ctx_impl, text_utf8, volume, rate); });
}
sao_sdk_status_t SAO_SDK_CALL tts_stop_boundary(void* ctx_impl) noexcept {
    return invoke_callback_barrier([&] { return tts_stop(ctx_impl); });
}
const SaoSdkTtsTable kTtsTable = {tts_speak_boundary, tts_stop_boundary};

sao_sdk_status_t SAO_SDK_CALL banner_show(void* ctx_impl, const char* text_utf8,
                                          uint32_t duration_ms, uint32_t argb_color) {
    ContextApiLease lease(cast_ctx(ctx_impl));
    if (!lease)
        return lease.status();
    auto* state = cast_ctx(ctx_impl);
    if (state == nullptr)
        return SAO_SDK_ERR_HANDLE_INVALID;
    if (text_utf8 == nullptr || state->bound_public_ctx == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    SaoSdkNotifySpec spec{};
    spec.text_utf8 = text_utf8;
    spec.duration_ms = duration_ms;
    spec.argb_color = argb_color;
    sao_sdk_notify_token_t token = 0;
    return sao_sdk_notify_show(state->bound_public_ctx, &spec, &token);
}
sao_sdk_status_t SAO_SDK_CALL banner_show_boundary(void* ctx_impl, const char* text_utf8,
                                                   uint32_t duration_ms,
                                                   uint32_t argb_color) noexcept {
    return invoke_callback_barrier(
        [&] { return banner_show(ctx_impl, text_utf8, duration_ms, argb_color); });
}
const SaoSdkBannerTable kBannerTable = {banner_show_boundary};

} // namespace

const SaoSdkMemTable* make_mem_table() {
    return &kMemTable;
}
const SaoSdkConfigTable* make_config_table() {
    return &kConfigTable;
}
const SaoSdkTtsTable* make_tts_table() {
    return &kTtsTable;
}
const SaoSdkBannerTable* make_banner_table() {
    return &kBannerTable;
}

const SaoSdkGpuHuntTable* make_public_gpu_hunt_table() {
    static const SaoSdkGpuHuntTable table = [] {
        SaoSdkGpuHuntTable value = *make_gpu_hunt_table();
        value.abi_version = SAO_SDK_GPU_HUNT_TABLE_ABI_VERSION;
        value.struct_size = sizeof(SaoSdkGpuHuntTable);
        return value;
    }();
    return &table;
}

void populate_context(ContextState* state, SaoSdkContext* out_ctx,
                      const char* plugin_version_utf8) {
    std::memset(out_ctx, 0, sizeof(*out_ctx));
    state->plugin_version = plugin_version_utf8 == nullptr ? "" : plugin_version_utf8;
    state->bound_public_ctx = out_ctx;
    out_ctx->abi_version = SAO_SDK_ABI_VERSION;
    out_ctx->ctx_impl = state;
    out_ctx->plugin_id_utf8 = state->plugin_id.c_str();
    out_ctx->plugin_version_utf8 = state->plugin_version.c_str();
    out_ctx->ui = make_ui_table();
    out_ctx->event = make_event_table();
    out_ctx->mem = make_mem_table();
    out_ctx->net = make_net_table();
    out_ctx->config = make_config_table();
    out_ctx->hotkey = make_hotkey_table();
    out_ctx->tts = make_tts_table();
    out_ctx->banner = make_banner_table();
    out_ctx->gpu_hunt = make_public_gpu_hunt_table();
}

} // namespace sao_sdk_internal

// ─── Context lifecycle exports ───────────────────────────────────────

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_bind_ui_compositor(void* compositor) {
    try {
        return sao_sdk_internal::SharedRuntime::instance().bind_compositor(
            static_cast<sao_ui_compositor_handle_t>(compositor));
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_unbind_ui_compositor(void) {
    try {
        return sao_sdk_internal::SharedRuntime::instance().unbind_compositor();
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_platform_get_ui_compositor(void** out_compositor) {
    if (out_compositor == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_compositor = nullptr;
    try {
        sao_ui_compositor_handle_t compositor = nullptr;
        const sao_sdk_status_t status =
            sao_sdk_internal::SharedRuntime::instance().get_bound_compositor(&compositor);
        if (status == SAO_SDK_OK)
            *out_compositor = compositor;
        return status;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

#if defined(SAO_SDK_TESTING)
extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_runtime_state(
    void** out_compositor, void** out_input_router, bool* out_owns_compositor,
    bool* out_compositor_bound, size_t* out_active_contexts) {
    if (out_compositor == nullptr || out_input_router == nullptr || out_owns_compositor == nullptr ||
        out_compositor_bound == nullptr || out_active_contexts == nullptr) {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }
    *out_compositor = nullptr;
    *out_input_router = nullptr;
    *out_owns_compositor = false;
    *out_compositor_bound = false;
    *out_active_contexts = 0;
    try {
        auto& runtime = sao_sdk_internal::SharedRuntime::instance();
        std::lock_guard lock(runtime.mu);
        *out_compositor = runtime.compositor;
        *out_input_router = runtime.input_router;
        *out_owns_compositor = runtime.owns_compositor;
        *out_compositor_bound = runtime.compositor_bound;
        *out_active_contexts = runtime.active_contexts;
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_test_reset_runtime(void) {
    try {
        auto& runtime = sao_sdk_internal::SharedRuntime::instance();
        std::lock_guard lock(runtime.mu);
        if (runtime.active_contexts != 0 || runtime.compositor_bound)
            return SAO_SDK_ERR_BUSY;
        if (runtime.owns_compositor && runtime.compositor != nullptr) {
            const sao_status_t preflight_status =
                sao_ui_compositor_destroy_preflight(runtime.compositor);
            if (preflight_status != SAO_STATUS_OK)
                return sao_sdk_internal::map_runtime_status(preflight_status);
        }
        auto* const previous_compositor = runtime.compositor;
        if (runtime.input_router != nullptr) {
            const sao_status_t router_status =
                sao_ui_input_router_deep_try_destroy(runtime.input_router);
            if (router_status != SAO_STATUS_OK)
                return sao_sdk_internal::map_runtime_status(router_status);
            runtime.input_router = nullptr;
        }
        if (runtime.owns_compositor && previous_compositor != nullptr) {
            const sao_status_t compositor_status =
                sao_ui_compositor_try_destroy(previous_compositor);
            if (compositor_status != SAO_STATUS_OK) {
                (void)sao_ui_input_router_deep_create(previous_compositor,
                                                      &runtime.input_router);
                return sao_sdk_internal::map_runtime_status(compositor_status);
            }
        }
        runtime.input_router = nullptr;
        runtime.compositor = nullptr;
        runtime.owns_compositor = false;
        runtime.compositor_bound = false;
        runtime.compositor_owner_thread = {};
        return SAO_SDK_OK;
    } catch (...) {
        return SAO_SDK_ERR_INTERNAL;
    }
}
#endif

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_context_create(
    const char* base_dir_utf8, const char* plugin_id_utf8, struct SaoSdkContext** out_ctx) {
    if (out_ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_ctx = nullptr;
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    sao_sdk_internal::ContextState* state = nullptr;
    bool registered = false;
    bool runtime_acquired = false;
    try {
        const sao_sdk_status_t runtime_status =
            sao_sdk_internal::SharedRuntime::instance().acquire_context();
        if (runtime_status != SAO_SDK_OK)
            return runtime_status;
        runtime_acquired = true;

        state = new (std::nothrow) sao_sdk_internal::ContextState();
        if (state == nullptr) {
            sao_sdk_internal::SharedRuntime::instance().release_context();
            return SAO_SDK_ERR_NOT_INITIALIZED;
        }
        state->runtime_context_acquired = true;
        runtime_acquired = false;
        state->plugin_id = plugin_id_utf8;
        state->base_dir = (base_dir_utf8 == nullptr) ? "" : base_dir_utf8;

        SaoSdkContext& pub = state->public_ctx;
        sao_sdk_internal::populate_context(state, &pub, "0.0.0");

        sao_sdk_internal::register_context(state);
        registered = true;
        const auto provider_status = sao_sdk_internal::bind_process_providers(state);
        if (provider_status != SAO_SDK_OK) {
            if (sao_sdk_context_try_destroy(&state->public_ctx) != SAO_SDK_OK)
                sao_sdk_internal::quarantine_context(state);
            return provider_status;
        }
        *out_ctx = &state->public_ctx;
        return SAO_SDK_OK;
    } catch (...) {
        if (state != nullptr) {
            if (registered) {
                if (sao_sdk_context_try_destroy(&state->public_ctx) != SAO_SDK_OK)
                    sao_sdk_internal::quarantine_context(state);
            } else {
                if (state->runtime_context_acquired) {
                    sao_sdk_internal::SharedRuntime::instance().release_context();
                    state->runtime_context_acquired = false;
                }
                delete state;
            }
        } else if (runtime_acquired) {
            sao_sdk_internal::SharedRuntime::instance().release_context();
        }
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_try_destroy(struct SaoSdkContext* ctx) {
    sao_sdk_internal::ContextState* state = nullptr;
    std::unique_lock<std::mutex> destroy_lock;
    try {
        const auto preflight_status =
            sao_sdk_internal::begin_context_shutdown(ctx, &state, &destroy_lock);
        if (preflight_status != SAO_SDK_OK)
            return preflight_status;
        const bool caller_owns_context = ctx != &state->public_ctx;

        const auto fail = [state](sao_sdk_status_t status) {
            sao_sdk_internal::cancel_context_shutdown(state);
            return status;
        };

        // Provider-owned registrations are swept in exact reverse registration
        // order before local event/panel state is released.
        const auto net_status = sao_sdk_internal::net_provider_cleanup(state);
        if (net_status != SAO_SDK_OK)
            return fail(net_status);
        const auto memory_status = sao_sdk_internal::memory_provider_cleanup(state);
        if (memory_status != SAO_SDK_OK)
            return fail(memory_status);
        const auto provider_status = sao_sdk_internal::provider_cleanup(state);
        if (provider_status != SAO_SDK_OK)
            return fail(provider_status);
        const auto gpu_status = sao_sdk_internal::sdk_gpu_hunt_sweep_owner(state);
        if (gpu_status != SAO_SDK_OK)
            return fail(gpu_status);

        sao_sdk_internal::cleanup_event_subscriptions(state);
        const auto panel_status = sao_sdk_internal::cleanup_ui_panels(state);
        if (panel_status != SAO_SDK_OK)
            return fail(panel_status);

        sao_sdk_internal::unregister_context(state);
        if (state->runtime_context_acquired) {
            sao_sdk_internal::SharedRuntime::instance().release_context();
            state->runtime_context_acquired = false;
        }
        if (caller_owns_context) {
            std::memset(ctx, 0, sizeof(*ctx));
        }
        sao_sdk_internal::g_context_destroy_owner = nullptr;
        destroy_lock.unlock();
        delete state;
        return SAO_SDK_OK;
    } catch (...) {
        if (state != nullptr) {
            sao_sdk_internal::cancel_context_shutdown(state);
            sao_sdk_internal::quarantine_context(state);
        }
        return SAO_SDK_ERR_INTERNAL;
    }
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_context_destroy(struct SaoSdkContext* ctx) {
    if (ctx == nullptr)
        return;
    try {
        const auto status = sao_sdk_context_try_destroy(ctx);
        if (status != SAO_SDK_OK)
            sao_sdk_internal::quarantine_public_context(ctx);
    } catch (...) {
        sao_sdk_internal::quarantine_public_context(ctx);
    }
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_get_plugin_id(const struct SaoSdkContext* ctx, const char** out_plugin_id_utf8) {
    if (out_plugin_id_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_plugin_id_utf8 = nullptr;
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    *out_plugin_id_utf8 = lease.state()->plugin_id.c_str();
    return SAO_SDK_OK;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL
sao_sdk_context_get_base_dir(const struct SaoSdkContext* ctx, const char** out_base_dir_utf8) {
    if (out_base_dir_utf8 == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    *out_base_dir_utf8 = nullptr;
    sao_sdk_internal::ContextApiLease lease(ctx);
    if (!lease)
        return lease.status();
    *out_base_dir_utf8 = lease.state()->base_dir.c_str();
    return SAO_SDK_OK;
}

#if defined(SAO_SDK_TESTING)
extern "C" SAO_SDK_API size_t SAO_SDK_CALL sao_sdk_test_live_context_count(void) {
    return sao_sdk_internal::live_context_count();
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_arm_context_api_pause(uint32_t point) {
    sao_sdk_internal::arm_context_api_test_pause(
        static_cast<sao_sdk_internal::ContextApiTestPoint>(point));
}

extern "C" SAO_SDK_API bool SAO_SDK_CALL
sao_sdk_test_wait_for_context_api_pause(uint32_t point) {
    return sao_sdk_internal::wait_for_context_api_test_pause(
        static_cast<sao_sdk_internal::ContextApiTestPoint>(point));
}

extern "C" SAO_SDK_API void SAO_SDK_CALL sao_sdk_test_resume_context_api_pause(uint32_t point) {
    sao_sdk_internal::resume_context_api_test_pause(
        static_cast<sao_sdk_internal::ContextApiTestPoint>(point));
}

extern "C" SAO_SDK_API bool SAO_SDK_CALL
sao_sdk_test_wait_for_context_shutdown(const struct SaoSdkContext* ctx) {
    return sao_sdk_internal::wait_for_context_shutdown(ctx);
}
#endif
