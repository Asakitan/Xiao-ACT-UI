#include "cs_component_internal.h"
#include "cs_sdk_bridge_internal.h"

#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/plugins/script_ctx/ctx_surface.h"
#include "sao/plugins/script_ctx/runtime_bridge.h"
#include "sao/plugins/script_ctx/script_ui.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/core/status.h"
#if defined(SAO_CSHARP_HAS_UI)
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/ui/compositor.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace sao::plugins::csharp_host {
namespace {

using loader::plugin_context_t;
using sdk_binding::language_binding_operation;
using sdk_binding::language_binding_request;
using sdk_binding::language_host_kind;
using sdk_binding::sdk_context_call_request;
using sdk_binding::sdk_method_id;

static_assert(static_cast<uint16_t>(sdk_method_id::method_set_compositor_layer_mmf_source) == 51);
static_assert(static_cast<uint16_t>(sdk_method_id::method_set_compositor_layer_shared_texture_source) == 52);
static_assert(static_cast<uint16_t>(sdk_method_id::method_compositor_gpu_interop_available) == 57);
static_assert(static_cast<uint16_t>(sdk_method_id::method_compositor_layer_shared_texture_active) == 58);

static_assert(sizeof(cs_managed_entity_snapshot_invocation_v2) == 48);
static_assert(sizeof(cs_managed_entity_menu_row_v2) == sizeof(loader::entity_menu_row_v2));
static_assert(alignof(cs_managed_entity_menu_row_v2) == alignof(loader::entity_menu_row_v2));
static_assert(offsetof(cs_managed_entity_menu_row_v2, struct_size) ==
              offsetof(loader::entity_menu_row_v2, struct_size));
static_assert(offsetof(cs_managed_entity_menu_row_v2, category_id_utf8) ==
              offsetof(loader::entity_menu_row_v2, category_id_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, category_label_utf8) ==
              offsetof(loader::entity_menu_row_v2, category_label_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, category_icon_utf8) ==
              offsetof(loader::entity_menu_row_v2, category_icon_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, category_priority) ==
              offsetof(loader::entity_menu_row_v2, category_priority));
static_assert(offsetof(cs_managed_entity_menu_row_v2, row_label_utf8) ==
              offsetof(loader::entity_menu_row_v2, row_label_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, row_icon_utf8) ==
              offsetof(loader::entity_menu_row_v2, row_icon_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, action_id_utf8) ==
              offsetof(loader::entity_menu_row_v2, action_id_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, payload_json_utf8) ==
              offsetof(loader::entity_menu_row_v2, payload_json_utf8));
static_assert(offsetof(cs_managed_entity_menu_row_v2, can_activate) ==
              offsetof(loader::entity_menu_row_v2, can_activate));
static_assert(offsetof(cs_managed_entity_menu_row_v2, reserved) ==
              offsetof(loader::entity_menu_row_v2, reserved));

struct managed_callback;
struct prompt_wait_state;

struct entity_callback_pair {
    void* snapshot = nullptr;
    void* action = nullptr;
    void* action_v2 = nullptr;
};

// Per-thread pending action-v2 sink slot. Populated by callback_entity_action_v2
// immediately before invoking the managed handler and consumed by
// table_submit_action_result_v2. The token is a stable pointer into the
// enclosing native invocation frame so the managed side can carry it opaquely
// through the invocation struct without leaking sink pointers to caller code.
struct pending_action_result {
    void* token = nullptr;
    loader::entity_action_result_sink_v2_fn sink = nullptr;
    void* sink_user_data = nullptr;
    bool consumed = false;
};

thread_local pending_action_result g_pending_action_result{};

struct entity_registration {
    std::string provider_id;
    std::string qualified_provider_id;
    std::unique_ptr<entity_callback_pair> callbacks;
};

static_assert(std::is_nothrow_move_constructible_v<entity_registration>);

} // namespace

// Per-timer binding payload pooled by the owning session for the full provider
// registration window. Blocks are never freed mid-flight so the provider can
// never observe a dangling user_data pointer; one-shot blocks stay pooled
// after their single fire and are reclaimed with the session.
struct loader_timer_ud {
    void* managed_token = nullptr;
    loader::plugin_context_t* ctx = nullptr;
    bool one_shot = false;
    uint64_t managed_id = 0;
    std::string token;
};

struct loader_panel_ud {
    std::string surface;
    void* render_token = nullptr;
    void* action_token = nullptr;
};

struct sdk_bridge_session {
    std::mutex mutex;
    std::condition_variable idle;
    void* runtime = nullptr;
    plugin_context_t* loader_context = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    managed_component_s* component = nullptr;
    void* binding = nullptr;
    size_t active_calls = 0;
    bool retiring = false;
    bool context_lease = false;
    uintptr_t handle = 0;
    int32_t callback_release_status = SAO_OK;
    std::string last_error;
    std::vector<managed_callback*> callbacks;
    std::vector<entity_registration> entities;
    std::vector<std::string> pending_entity_ids;
    // Loader-context dispatch bookkeeping. Keys "sub:<u32>", "timer:<token>",
    // "hook:<u32>", "dsrc:<id>", "ci:<name>" map to managed callback tokens so
    // explicit unregister paths release the matching wrapper early; session
    // teardown drains anything left through session->callbacks.
    std::unordered_map<std::string, void*> loader_bindings;
    std::vector<std::unique_ptr<loader_timer_ud>> timer_ud_pool;
    std::vector<std::unique_ptr<loader_panel_ud>> panel_ud_pool;
    std::vector<std::shared_ptr<prompt_wait_state>> prompt_states;
    bool prompt_active = false;
};

namespace {

struct managed_callback {
    sdk_bridge_session* owner = nullptr;
    cs_managed_callback_descriptor descriptor{};
    uintptr_t token = 0;
    size_t active_calls = 0;
    bool release_pending = false;
};

std::mutex g_sessions_mutex;
std::unordered_map<void*, sdk_bridge_session*> g_sessions;
std::unordered_map<uintptr_t, sdk_bridge_session*> g_session_handles;
std::atomic<uintptr_t> g_next_session_handle{1};
std::mutex g_callbacks_mutex;
std::unordered_map<uintptr_t, managed_callback*> g_callbacks;
std::atomic<uintptr_t> g_next_callback_token{1};
constexpr size_t kMaximumSessionCallNesting = 64;
thread_local std::array<sdk_bridge_session*, kMaximumSessionCallNesting> g_active_sessions{};
thread_local size_t g_active_session_depth = 0;
thread_local bool g_prompt_wait_active = false;

bool session_active_on_current_thread(const sdk_bridge_session* session) noexcept {
    return std::find(g_active_sessions.begin(), g_active_sessions.begin() + g_active_session_depth,
                     session) != g_active_sessions.begin() + g_active_session_depth;
}

bool push_active_session(sdk_bridge_session* session) noexcept {
    if (g_active_session_depth == kMaximumSessionCallNesting)
        return false;
    g_active_sessions[g_active_session_depth++] = session;
    return true;
}

void pop_active_session(sdk_bridge_session* session) noexcept {
    if (g_active_session_depth > 0 && g_active_sessions[g_active_session_depth - 1] == session)
        g_active_sessions[--g_active_session_depth] = nullptr;
}

class session_call_lease {
  public:
    int32_t acquire(sdk_bridge_session* session) noexcept {
        if (session == nullptr)
            return SAO_ERR_INVALID_ARGUMENT;
        try {
            std::lock_guard lock(session->mutex);
            if (session->retiring)
                return loader::SAO_PLUGINS_ERR_BUSY;
            if (!push_active_session(session))
                return loader::SAO_PLUGINS_ERR_BUSY;
            ++session->active_calls;
            session_ = session;
            return SAO_OK;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    int32_t acquire_handle(cs_managed_sdk_session_t opaque) noexcept {
        const uintptr_t handle = reinterpret_cast<uintptr_t>(opaque);
        if (handle == 0)
            return SAO_ERR_INVALID_ARGUMENT;
        try {
            std::lock_guard lock(g_sessions_mutex);
            const auto found = g_session_handles.find(handle);
            return found == g_session_handles.end() ? SAO_ERR_HANDLE_INVALID
                                                    : acquire(found->second);
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }

    ~session_call_lease() {
        if (session_ == nullptr)
            return;
        try {
            pop_active_session(session_);
            std::lock_guard lock(session_->mutex);
            if (session_->active_calls > 0)
                --session_->active_calls;
            if (session_->active_calls == 0)
                session_->idle.notify_all();
        } catch (...) {
        }
    }

    sdk_bridge_session* get() const noexcept {
        return session_;
    }

  private:
    sdk_bridge_session* session_ = nullptr;
};

int32_t acquire_runtime_session(void* runtime, session_call_lease& lease) noexcept {
    if (runtime == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(g_sessions_mutex);
        const auto found = g_sessions.find(runtime);
        return found == g_sessions.end() ? SAO_ERR_HANDLE_INVALID : lease.acquire(found->second);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

uintptr_t next_callback_token() noexcept {
    uintptr_t token = g_next_callback_token.fetch_add(1, std::memory_order_relaxed);
    while (token == 0)
        token = g_next_callback_token.fetch_add(1, std::memory_order_relaxed);
    return token;
}

uintptr_t next_session_handle() noexcept {
    uintptr_t handle = g_next_session_handle.fetch_add(1, std::memory_order_relaxed);
    while (handle == 0)
        handle = g_next_session_handle.fetch_add(1, std::memory_order_relaxed);
    return handle;
}

void remember_error(sdk_bridge_session* session, int32_t status, const char* operation) noexcept {
    if (session == nullptr || status == SAO_OK)
        return;
    try {
        std::lock_guard lock(session->mutex);
        session->last_error =
            std::string(operation) + " failed with status " + std::to_string(status);
    } catch (...) {
    }
}

struct managed_barrier_call {
    managed_callback* callback;
    const void* invocation;
};

int32_t SAO_PLUGINS_CALL invoke_managed(void* opaque) {
    auto* call = static_cast<managed_barrier_call*>(opaque);
    return call->callback->descriptor.invoke(call->callback->descriptor.gc_handle,
                                             call->callback->descriptor.kind, call->invocation);
}

struct managed_lifetime_call {
    managed_callback* callback;
    bool retain;
};

int32_t SAO_PLUGINS_CALL invoke_managed_lifetime(void* opaque) {
    auto* call = static_cast<managed_lifetime_call*>(opaque);
    return call->retain ? call->callback->descriptor.retain(call->callback->descriptor.gc_handle)
                        : call->callback->descriptor.release(call->callback->descriptor.gc_handle);
}

void release_managed_callback(void* opaque_token) noexcept;

int32_t invoke_callback(void* opaque_token, const void* invocation) noexcept {
    const uintptr_t token = reinterpret_cast<uintptr_t>(opaque_token);
    if (token == 0)
        return SAO_ERR_INVALID_ARGUMENT;
    managed_callback* callback = nullptr;
    sdk_bridge_session* session = nullptr;
    bool active = false;
    try {
        {
            std::lock_guard callback_lock(g_callbacks_mutex);
            const auto found = g_callbacks.find(token);
            if (found == g_callbacks.end())
                return SAO_ERR_HANDLE_INVALID;
            callback = found->second;
            session = callback->owner;
            std::lock_guard session_lock(session->mutex);
            if (session->retiring || callback->release_pending)
                return loader::SAO_PLUGINS_ERR_BUSY;
            if (!push_active_session(session))
                return loader::SAO_PLUGINS_ERR_BUSY;
            ++session->active_calls;
            ++callback->active_calls;
            active = true;
        }
        managed_barrier_call call{callback, invocation};
        const int32_t status =
            sdk_binding::sao_plugins_binding_barrier(invoke_managed, &call, nullptr);
        pop_active_session(session);
        bool release_pending = false;
        {
            std::lock_guard lock(session->mutex);
            --callback->active_calls;
            --session->active_calls;
            active = false;
            if (session->active_calls == 0)
                session->idle.notify_all();
            release_pending = callback->active_calls == 0 && callback->release_pending;
            if (status != SAO_OK) {
                session->last_error =
                    "managed callback failed with status " + std::to_string(status);
            }
        }
        if (release_pending)
            release_managed_callback(opaque_token);
        return status;
    } catch (...) {
        if (active && session != nullptr && callback != nullptr) {
            pop_active_session(session);
            try {
                std::lock_guard lock(session->mutex);
                if (callback->active_calls > 0)
                    --callback->active_calls;
                if (session->active_calls > 0)
                    --session->active_calls;
                if (session->active_calls == 0)
                    session->idle.notify_all();
            } catch (...) {
            }
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void release_managed_callback(void* opaque_token) noexcept {
    const uintptr_t token = reinterpret_cast<uintptr_t>(opaque_token);
    if (token == 0)
        return;
    managed_callback* callback = nullptr;
    sdk_bridge_session* session = nullptr;
    bool detached = false;
    try {
        {
            std::lock_guard callback_lock(g_callbacks_mutex);
            const auto registered = g_callbacks.find(token);
            if (registered == g_callbacks.end())
                return;
            callback = registered->second;
            session = callback->owner;
            std::lock_guard session_lock(session->mutex);
            const auto found =
                std::find(session->callbacks.begin(), session->callbacks.end(), callback);
            if (found == session->callbacks.end())
                return;
            if (callback->active_calls != 0) {
                callback->release_pending = true;
                return;
            }
            if (!push_active_session(session)) {
                session->callback_release_status = loader::SAO_PLUGINS_ERR_BUSY;
                return;
            }
            ++session->active_calls;
            session->callbacks.erase(found);
            g_callbacks.erase(registered);
            detached = true;
        }
        managed_lifetime_call call{callback, false};
        const int32_t status =
            sdk_binding::sao_plugins_binding_barrier(invoke_managed_lifetime, &call, nullptr);
        pop_active_session(session);
        {
            std::lock_guard lock(session->mutex);
            if (status != SAO_OK && session->callback_release_status == SAO_OK) {
                session->callback_release_status = status;
                session->last_error =
                    "managed callback release failed with status " + std::to_string(status);
            }
            --session->active_calls;
            if (session->active_calls == 0)
                session->idle.notify_all();
        }
        delete callback;
    } catch (...) {
        if (session != nullptr) {
            try {
                std::lock_guard lock(session->mutex);
                session->callback_release_status = SAO_ERR_OS_CALL_FAILED;
                if (detached) {
                    pop_active_session(session);
                }
                if (detached && session->active_calls > 0) {
                    --session->active_calls;
                    if (session->active_calls == 0)
                        session->idle.notify_all();
                }
            } catch (...) {
            }
        }
        if (detached)
            delete callback;
    }
}

void SAO_PLUGINS_CALL callback_event(const char* topic_utf8, const uint8_t* payload_json_utf8,
                                     size_t payload_size, void* user_data) {
    const cs_managed_event_invocation invocation{topic_utf8, payload_json_utf8, payload_size};
    (void)invoke_callback(user_data, &invocation);
}

void SAO_PLUGINS_CALL callback_hotkey(uint64_t hotkey_id, void* user_data) {
    const cs_managed_hotkey_invocation invocation{hotkey_id};
    (void)invoke_callback(user_data, &invocation);
}

void SAO_PLUGINS_CALL callback_timer(uint64_t timer_id, void* user_data) {
    const cs_managed_timer_invocation invocation{timer_id};
    (void)invoke_callback(user_data, &invocation);
}

void SAO_PLUGINS_CALL callback_panel_action(const char* action_key_utf8,
                                            const uint8_t* action_json_utf8, size_t action_size,
                                            void* user_data) {
    const cs_managed_panel_action_invocation invocation{action_key_utf8, action_json_utf8,
                                                        action_size};
    (void)invoke_callback(user_data, &invocation);
}

int32_t SAO_PLUGINS_CALL callback_entity_snapshot(loader::entity_menu_row* rows, uint32_t capacity,
                                                  uint32_t* out_count, uint64_t* out_revision,
                                                  void* user_data) {
    auto* pair = static_cast<entity_callback_pair*>(user_data);
    const cs_managed_entity_snapshot_invocation invocation{rows, capacity, out_count, out_revision};
    return pair == nullptr ? SAO_ERR_INVALID_ARGUMENT
                           : invoke_callback(pair->snapshot, &invocation);
}

int32_t SAO_PLUGINS_CALL callback_entity_snapshot_v2(
    void* rows, uint32_t capacity, uint32_t row_stride_bytes, uint32_t* out_count,
    uint64_t* out_revision, loader::entity_snapshot_content_token_t* out_content_token,
    uint32_t* out_row_stride_bytes, void* user_data) {
    auto* pair = static_cast<entity_callback_pair*>(user_data);
    if (pair == nullptr || out_count == nullptr || out_revision == nullptr ||
        out_content_token == nullptr || out_row_stride_bytes == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const cs_managed_entity_snapshot_invocation_v2 invocation{
        rows,         capacity,          row_stride_bytes,     out_count,
        out_revision, out_content_token, out_row_stride_bytes,
    };
    return invoke_callback(pair->snapshot, &invocation);
}

int32_t SAO_PLUGINS_CALL callback_entity_action(const char* action_id_utf8,
                                                const char* payload_json_utf8, void* user_data) {
    auto* pair = static_cast<entity_callback_pair*>(user_data);
    const cs_managed_entity_action_invocation invocation{action_id_utf8, payload_json_utf8};
    return pair == nullptr ? SAO_ERR_INVALID_ARGUMENT : invoke_callback(pair->action, &invocation);
}

// Direct-path forwarder that adapts the managed cs_managed_action_result_v2
// layout to the loader's entity_action_result_v2 sink call. The two structs
// share identical prefix layout (24-byte required prefix, ABI2 handled+result
// pointer); a static assertion at the entity_provider header lock guarantees
// this. sink_user_data is a pointer to the enclosing pending_action_result
// slot so double-submit and stale invocation calls are diagnosed here without
// touching loader-side state.
static_assert(sizeof(cs_managed_action_result_v2) == sizeof(loader::entity_action_result_v2));
static_assert(offsetof(cs_managed_action_result_v2, struct_size) ==
              offsetof(loader::entity_action_result_v2, struct_size));
static_assert(offsetof(cs_managed_action_result_v2, abi_version) ==
              offsetof(loader::entity_action_result_v2, abi_version));
static_assert(offsetof(cs_managed_action_result_v2, handled) ==
              offsetof(loader::entity_action_result_v2, handled));
static_assert(offsetof(cs_managed_action_result_v2, reserved) ==
              offsetof(loader::entity_action_result_v2, reserved));
static_assert(offsetof(cs_managed_action_result_v2, result_json_utf8) ==
              offsetof(loader::entity_action_result_v2, result_json_utf8));

int32_t SAO_PLUGINS_CALL forward_managed_action_result(const cs_managed_action_result_v2* result,
                                                       void* sink_user_data) {
    auto* pending = static_cast<pending_action_result*>(sink_user_data);
    if (pending == nullptr || pending->sink == nullptr || result == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (pending->consumed)
        return SAO_ERR_HANDLE_INVALID;
    const int32_t status = pending->sink(
        reinterpret_cast<const loader::entity_action_result_v2*>(result), pending->sink_user_data);
    if (status == SAO_OK)
        pending->consumed = true;
    return status;
}

// v3 action-v2 producer: forwards to the managed handler with the pending sink
// wired via a thread-local slot. The token is the address of the pending slot
// itself, which the managed side treats opaquely and passes back through the
// submit path. Managed callers may also invoke the exposed sink pointer in the
// invocation struct directly. Failure to submit before returning is a producer
// error and is surfaced by the loader through its normal action-v2 contract.
int32_t SAO_PLUGINS_CALL callback_entity_action_v2(
    const char* action_id_utf8, const char* payload_json_utf8,
    loader::entity_action_result_sink_v2_fn result_sink, void* result_sink_user_data,
    void* user_data) {
    auto* pair = static_cast<entity_callback_pair*>(user_data);
    if (pair == nullptr || result_sink == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    pending_action_result saved = g_pending_action_result;
    g_pending_action_result.token = &g_pending_action_result;
    g_pending_action_result.sink = result_sink;
    g_pending_action_result.sink_user_data = result_sink_user_data;
    g_pending_action_result.consumed = false;
    sdk_bridge_session* owning_session = nullptr;
    void* target_token = pair->action_v2;
    try {
        std::lock_guard callback_lock(g_callbacks_mutex);
        const auto found = g_callbacks.find(reinterpret_cast<uintptr_t>(target_token));
        if (found != g_callbacks.end() && found->second != nullptr)
            owning_session = found->second->owner;
    } catch (...) {
        g_pending_action_result = saved;
        return SAO_ERR_OS_CALL_FAILED;
    }
    const cs_managed_entity_action_invocation_v2 invocation{
        owning_session == nullptr ? nullptr
                                  : reinterpret_cast<void*>(owning_session->handle),
        g_pending_action_result.token,
        action_id_utf8,
        payload_json_utf8,
        forward_managed_action_result,
        &g_pending_action_result,
    };
    const int32_t status = invoke_callback(target_token, &invocation);
    g_pending_action_result = saved;
    return status;
}

// ── loader-context trampolines for canonical ctx bindings ─────────────────
// Each trampoline adapts one plugin_context_t callback signature to its
// kind-specific managed invocation and routes it through invoke_callback with
// the managed callback token as user_data. The new kinds never ride through
// the wrapped out_callback path; callback_entry maps them to their primary
// trampoline only so the pointer contract stays meaningful.

constexpr size_t kMaxRenderSpecJsonBytes = 1024u * 1024u;

void SAO_PLUGINS_CALL callback_loader_event(const char* topic_utf8,
                                            const char* event_json_utf8,
                                            void* user_data) {
    if (event_json_utf8 == nullptr) {
        event_json_utf8 = "{}";
    }
    const cs_managed_event_invocation invocation{
        topic_utf8, reinterpret_cast<const uint8_t*>(event_json_utf8),
        std::strlen(event_json_utf8)};
    (void)invoke_callback(user_data, &invocation);
}

void SAO_PLUGINS_CALL callback_loader_timer(void* user_data) {
    auto* ud = static_cast<loader_timer_ud*>(user_data);
    if (ud == nullptr) {
        return;
    }
    if (ud->one_shot && ud->ctx != nullptr && !ud->token.empty()) {
        // Canonical one-shot semantics: the loader token completes before the
        // managed handler runs so the registration cannot re-fire.
        (void)loader::sao_plugins_ctx_complete_timer(ud->ctx, ud->token.c_str());
    }
    const cs_managed_timer_invocation invocation{ud->managed_id};
    (void)invoke_callback(ud->managed_token, &invocation);
}

int32_t SAO_PLUGINS_CALL callback_data_source_start(void* user_data) {
    const cs_managed_data_source_invocation invocation{0, 0};
    return invoke_callback(user_data, &invocation);
}

int32_t SAO_PLUGINS_CALL callback_data_source_stop(void* user_data) {
    const cs_managed_data_source_invocation invocation{1, 0};
    return invoke_callback(user_data, &invocation);
}

int32_t SAO_PLUGINS_CALL callback_render_hook(const char* surface_utf8,
                                              const char* payload_json_utf8,
                                              char** out_spec_json_utf8,
                                              void* user_data) {
    if (out_spec_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_spec_json_utf8 = nullptr;
    // Two-budget capacity protocol: the managed side reports required bytes
    // through the invocation; grow once when the estimate under-runs, then
    // hand the loader a caller-freed copy in the ctx string family (new[]).
    size_t capacity = 64u * 1024u;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::unique_ptr<char[]> buffer(new (std::nothrow) char[capacity]);
        if (buffer == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        size_t required = 0;
        cs_managed_render_invocation invocation{surface_utf8, payload_json_utf8, buffer.get(),
                                                capacity, &required};
        const int32_t status = invoke_callback(user_data, &invocation);
        if (status != SAO_OK)
            return status;
        if (required == 0)
            return SAO_OK;
        if (required > kMaxRenderSpecJsonBytes)
            return SAO_ERR_INVALID_ARGUMENT;
        if (required <= capacity) {
            char* spec = new (std::nothrow) char[required + 1];
            if (spec == nullptr)
                return SAO_ERR_OS_CALL_FAILED;
            std::memcpy(spec, buffer.get(), required);
            spec[required] = '\0';
            *out_spec_json_utf8 = spec;
            return SAO_OK;
        }
        capacity = required + 1;
    }
    return SAO_ERR_OS_CALL_FAILED;
}

int32_t SAO_PLUGINS_CALL callback_loader_panel_render(const char* payload_json_utf8,
                                                      char** out_spec_json_utf8,
                                                      void* user_data) {
    auto* panel = static_cast<loader_panel_ud*>(user_data);
    if (panel == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    return callback_render_hook(panel->surface.c_str(), payload_json_utf8,
                                out_spec_json_utf8, panel->render_token);
}

int32_t SAO_PLUGINS_CALL callback_loader_panel_action(const char* action_utf8,
                                                      const char* payload_json_utf8,
                                                      char** out_result_json_utf8,
                                                      void* user_data) {
    if (out_result_json_utf8 == nullptr || user_data == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_result_json_utf8 = nullptr;
    auto* panel = static_cast<loader_panel_ud*>(user_data);
    const cs_managed_panel_action_invocation invocation{
        action_utf8, reinterpret_cast<const uint8_t*>(payload_json_utf8),
        payload_json_utf8 == nullptr ? 0 : std::strlen(payload_json_utf8)};
    return invoke_callback(panel->action_token, &invocation);
}

void SAO_PLUGINS_CALL callback_compositor_cursor_pos(float x, float y, void* user_data) {
    const cs_managed_compositor_input_invocation invocation{0, 0, x, y, 0, {}};
    (void)invoke_callback(user_data, &invocation);
}
void SAO_PLUGINS_CALL callback_compositor_mouse_button(uint32_t button, bool pressed,
                                                       void* user_data) {
    const cs_managed_compositor_input_invocation invocation{1, button, 0.0f, 0.0f,
                                                            static_cast<uint8_t>(pressed ? 1 : 0),
                                                            {}};
    (void)invoke_callback(user_data, &invocation);
}
void SAO_PLUGINS_CALL callback_compositor_cursor_leave(void* user_data) {
    const cs_managed_compositor_input_invocation invocation{2, 0, 0.0f, 0.0f, 0, {}};
    (void)invoke_callback(user_data, &invocation);
}
void SAO_PLUGINS_CALL callback_compositor_scroll(float dx, float dy, void* user_data) {
    const cs_managed_compositor_input_invocation invocation{3, 0, dx, dy, 0, {}};
    (void)invoke_callback(user_data, &invocation);
}

// Snapshot-only entity providers registered via the canonical
// register_menu_category dispatch carry no managed action handler; this stub
// submits a declined result so row activation fails closed instead of
// stalling the loader's action-v2 contract. Full action plumbing stays
// available through the register_entity_provider_v2 table slot.
int32_t SAO_PLUGINS_CALL decline_entity_action_v2(
    const char* action_id_utf8, const char* payload_json_utf8,
    loader::entity_action_result_sink_v2_fn result_sink, void* result_sink_user_data,
    void* user_data) {
    (void)action_id_utf8;
    (void)payload_json_utf8;
    (void)user_data;
    if (result_sink == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    loader::entity_action_result_v2 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 2;
    result.handled = 0;
    result.result_json_utf8 = nullptr;
    return result_sink(&result, result_sink_user_data);
}

void* callback_entry(cs_managed_callback_kind kind) noexcept {
    switch (kind) {
    case cs_managed_callback_kind::event:
        return reinterpret_cast<void*>(&callback_event);
    case cs_managed_callback_kind::hotkey:
        return reinterpret_cast<void*>(&callback_hotkey);
    case cs_managed_callback_kind::timer:
        return reinterpret_cast<void*>(&callback_timer);
    case cs_managed_callback_kind::panel_action:
        return reinterpret_cast<void*>(&callback_panel_action);
    case cs_managed_callback_kind::entity_snapshot:
    case cs_managed_callback_kind::entity_action:
    case cs_managed_callback_kind::entity_action_v2:
        return reinterpret_cast<void*>(&invoke_managed);
    case cs_managed_callback_kind::render_hook:
        return reinterpret_cast<void*>(&callback_render_hook);
    case cs_managed_callback_kind::data_source:
        return reinterpret_cast<void*>(&callback_data_source_start);
    case cs_managed_callback_kind::compositor_input:
        return reinterpret_cast<void*>(&callback_compositor_cursor_pos);
    }
    return nullptr;
}

bool valid_callback_descriptor(const cs_managed_callback_descriptor* descriptor) noexcept {
    return descriptor != nullptr &&
           descriptor->struct_size >= sizeof(cs_managed_callback_descriptor) &&
           descriptor->abi_version == SAO_CSHOST_SDK_TABLE_ABI_VERSION &&
           descriptor->gc_handle != nullptr && descriptor->invoke != nullptr &&
           descriptor->retain != nullptr && descriptor->release != nullptr &&
           callback_entry(descriptor->kind) != nullptr;
}

int32_t wrap_managed_callback(sdk_bridge_session* session,
                              const cs_managed_callback_descriptor* descriptor, void** out_callback,
                              void** out_user_data) noexcept {
    if (session == nullptr || !valid_callback_descriptor(descriptor) || out_callback == nullptr ||
        out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    *out_callback = nullptr;
    *out_user_data = nullptr;
    language_binding_request request{};
    request.runtime = session->runtime;
    request.value = const_cast<cs_managed_callback_descriptor*>(descriptor);
    request.out_callback = out_callback;
    request.out_user_data = out_user_data;
    return sdk_binding::sao_plugins_binding_dispatch_provider(
        language_host_kind::csharp, language_binding_operation::callback_wrap, &request);
}

void release_callback_token(void* token) noexcept {
    if (token != nullptr)
        sdk_binding::sao_plugins_binding_csharp_release_delegate(token);
}

// ── canonical ctx surface: loader-context dispatch ────────────────────────
//
// sao_plugins_sdk_context_dispatch intentionally covers only the SDK-domain
// subset of the canonical v1 ctx names (props, settings, hotkey, ui panel,
// overlay, time).  Everything else — the event/snapshot domain, timers,
// notifications, dialogs, compositor layers, data sources, entity menus,
// engine lookup, requirements and load_local — lives on the loader
// plugin_context_t ABI (the same layer the python host prefers).  The two
// switches below own every canonical name that has a loader-context
// counterpart (or must fail closed) while SDK-domain ids fall through to the
// existing dispatch unchanged.  Results follow the binding convention: raw
// JSON values serialized into out_result_json_utf8.

using ordered_json = nlohmann::ordered_json;

constexpr int32_t kDispatchFallthrough = (std::numeric_limits<int32_t>::min)();
constexpr size_t kMaxDispatchArgsBytes = 64u * 1024u * 1024u;
constexpr auto kPromptWaitBudget = std::chrono::seconds(300);

int32_t write_raw_result(const char* data, size_t size, char* out, size_t capacity,
                         size_t* required) noexcept {
    if (required == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *required = size + 1;
    if (out == nullptr || capacity < *required) {
        if (out != nullptr && capacity > 0)
            out[0] = '\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    if (size > 0)
        std::memcpy(out, data, size);
    out[size] = '\0';
    return SAO_OK;
}

int32_t write_call_result(const std::string& serialized, cs_managed_sdk_call* call) noexcept {
    try {
        return write_raw_result(serialized.data(), serialized.size(),
                                call->out_result_json_utf8, call->out_capacity,
                                call->out_required);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t write_call_json(const ordered_json& value, cs_managed_sdk_call* call) noexcept {
    try {
        return write_call_result(value.dump(), call);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

const ordered_json* arg_member(const ordered_json& args, const char* key) noexcept {
    if (key == nullptr || !args.is_object())
        return nullptr;
    const auto it = args.find(key);
    return it == args.end() ? nullptr : &*it;
}

std::string arg_utf8(const ordered_json& args, const char* key,
                     const std::string& fallback = "") {
    const ordered_json* value = arg_member(args, key);
    if (value == nullptr)
        return fallback;
    try {
        if (value->is_string())
            return value->get<std::string>();
        if (value->is_number_integer())
            return std::to_string(value->get<int64_t>());
        if (value->is_number_unsigned())
            return std::to_string(value->get<uint64_t>());
    } catch (...) {
    }
    return fallback;
}

double arg_f64(const ordered_json& args, const char* key, double fallback = 0.0) noexcept {
    const ordered_json* value = arg_member(args, key);
    if (value == nullptr || !value->is_number())
        return fallback;
    try {
        return value->get<double>();
    } catch (...) {
        return fallback;
    }
}

int64_t arg_i64(const ordered_json& args, const char* key, int64_t fallback = 0) noexcept {
    const ordered_json* value = arg_member(args, key);
    if (value == nullptr || !value->is_number())
        return fallback;
    try {
        return value->get<int64_t>();
    } catch (...) {
        return fallback;
    }
}

uint32_t arg_u32(const ordered_json& args, const char* key, uint32_t fallback = 0) noexcept {
    const int64_t value = arg_i64(args, key, fallback);
    return value < 0 ? fallback : static_cast<uint32_t>(value);
}

bool arg_bool(const ordered_json& args, const char* key, bool fallback = false) noexcept {
    const ordered_json* value = arg_member(args, key);
    if (value == nullptr)
        return fallback;
    try {
        if (value->is_boolean())
            return value->get<bool>();
        if (value->is_number())
            return value->get<int64_t>() != 0;
    } catch (...) {
    }
    return fallback;
}

uint64_t fnv1a64(const std::string& text) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(uint64_t value) {
    const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i)
        out[static_cast<size_t>(i)] = digits[(value >> ((15 - i) * 4)) & 0xF];
    return out;
}

std::wstring wide_from_utf8(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            cp = 0xFFFD;
        }
        if (i + len > text.size()) {
            cp = 0xFFFD;
            len = 1;
        }
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cont = static_cast<unsigned char>(text[i + k]);
            cp = (cont & 0xC0) == 0x80 ? (cp << 6) | (cont & 0x3F) : 0xFFFD;
            if (cp == 0xFFFD)
                break;
        }
        i += len;
        if (cp > 0x10FFFF)
            cp = 0xFFFD;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

std::string utf8_from_wide(const std::wstring& text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = static_cast<uint16_t>(text[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
            const uint32_t lo = static_cast<uint16_t>(text[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

int8_t base64_value(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z')
        return static_cast<int8_t>(c - 'A');
    if (c >= 'a' && c <= 'z')
        return static_cast<int8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9')
        return static_cast<int8_t>(c - '0' + 52);
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

bool base64_decode(const std::string& input, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve((input.size() / 4) * 3 + 3);
    uint32_t acc = 0;
    int bits = 0;
    bool padding = false;
    for (const unsigned char c : input) {
        if (c == '=') {
            padding = true;
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (padding)
            return false;
        const int8_t value = base64_value(c);
        if (value < 0)
            return false;
        acc = (acc << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
            acc &= (1u << bits) - 1u;
        }
    }
    return true;
}

bool parse_call_args(const cs_managed_sdk_call* call, ordered_json& args) noexcept {
    args = ordered_json::object();
    if (call == nullptr || call->args_json_utf8 == nullptr || call->args_size == 0)
        return true;
    if (call->args_size > kMaxDispatchArgsBytes)
        return false;
    try {
        args = ordered_json::parse(call->args_json_utf8,
                                   call->args_json_utf8 + call->args_size);
        return args.is_object() || args.is_array();
    } catch (...) {
        return false;
    }
}

void put_loader_binding(sdk_bridge_session* session, const std::string& key,
                        void* managed_token) noexcept {
    try {
        std::lock_guard lock(session->mutex);
        session->loader_bindings[key] = managed_token;
    } catch (...) {
    }
}

void* take_loader_binding(sdk_bridge_session* session, const std::string& key) noexcept {
    try {
        std::lock_guard lock(session->mutex);
        const auto found = session->loader_bindings.find(key);
        if (found == session->loader_bindings.end())
            return nullptr;
        void* token = found->second;
        session->loader_bindings.erase(found);
        return token;
    } catch (...) {
        return nullptr;
    }
}

void release_loader_binding(sdk_bridge_session* session, const std::string& key) noexcept {
    void* token = take_loader_binding(session, key);
    if (token != nullptr)
        release_callback_token(token);
}

const char* fixed_event_topic(sdk_method_id method) noexcept {
    switch (method) {
    case sdk_method_id::method_on_damage:
        return "damage";
    case sdk_method_id::method_on_heal:
        return "heal";
    case sdk_method_id::method_on_skill:
        return "skill";
    case sdk_method_id::method_on_boss:
        return "boss";
    case sdk_method_id::method_on_snapshot:
        return "act_snapshot";
    case sdk_method_id::method_on_encounter_finalized:
        return "encounter_finalized";
    default:
        return nullptr;
    }
}

const char* extension_kind_for(sdk_method_id method) noexcept {
    switch (method) {
    case sdk_method_id::method_register_parser_adapter:
        return "parser_adapter";
    case sdk_method_id::method_register_exporter:
        return "exporter";
    case sdk_method_id::method_register_formatter:
        return "formatter";
    case sdk_method_id::method_register_trigger_type:
        return "trigger_type";
    case sdk_method_id::method_register_report_view:
        return "report_view";
    case sdk_method_id::method_register_timer:
        return "timer";
    default:
        return nullptr;
    }
}

// Method ids that belong to the loader plugin_context_t domain (or must fail
// closed there). Everything else falls through to the SDK-context dispatch.
bool loader_ctx_owns_method(sdk_method_id method) noexcept {
    switch (method) {
    case sdk_method_id::prop_should_stop:
    case sdk_method_id::method_subscribe:
    case sdk_method_id::method_subscribe_once:
    case sdk_method_id::method_unsubscribe:
    case sdk_method_id::method_on_damage:
    case sdk_method_id::method_on_heal:
    case sdk_method_id::method_on_skill:
    case sdk_method_id::method_on_boss:
    case sdk_method_id::method_on_snapshot:
    case sdk_method_id::method_on_encounter_finalized:
    case sdk_method_id::method_emit:
    case sdk_method_id::method_get_snapshot:
    case sdk_method_id::method_snapshot_value:
    case sdk_method_id::method_recent_events:
    case sdk_method_id::method_register_parser_adapter:
    case sdk_method_id::method_register_exporter:
    case sdk_method_id::method_register_formatter:
    case sdk_method_id::method_register_trigger_type:
    case sdk_method_id::method_register_report_view:
    case sdk_method_id::method_register_timer:
    case sdk_method_id::method_register_ui_panel:
    case sdk_method_id::method_register_render_hook:
    case sdk_method_id::method_request_redraw:
    case sdk_method_id::method_register_data_source:
    case sdk_method_id::method_register_menu_category:
    case sdk_method_id::method_register_menu_surface:
    case sdk_method_id::method_register_action_handler:
    case sdk_method_id::method_set_interval:
    case sdk_method_id::method_set_timeout:
    case sdk_method_id::method_clear_timer:
    case sdk_method_id::method_run_on_ui:
    case sdk_method_id::method_notify:
    case sdk_method_id::method_dismiss_notify:
    case sdk_method_id::method_toast:
    case sdk_method_id::method_open_file:
    case sdk_method_id::method_open_window:
    case sdk_method_id::method_create_compositor_layer:
    case sdk_method_id::method_upload_compositor_frame:
    case sdk_method_id::method_set_compositor_layer_mmf_source:
    case sdk_method_id::method_set_compositor_layer_shared_texture_source:
    case sdk_method_id::method_set_compositor_layer_position:
    case sdk_method_id::method_set_compositor_layer_visible:
    case sdk_method_id::method_set_compositor_layer_input:
    case sdk_method_id::method_destroy_compositor_layer:
    case sdk_method_id::method_compositor_gpu_interop_available:
    case sdk_method_id::method_compositor_layer_shared_texture_active:
    case sdk_method_id::method_compositor_display_refresh_hz:
    case sdk_method_id::method_get_engine:
    case sdk_method_id::method_require_engine:
    case sdk_method_id::method_call_engine:
    case sdk_method_id::method_call_runtime:
    case sdk_method_id::method_ensure_requirements:
    case sdk_method_id::method_load_local:
    case sdk_method_id::method_register_engine:
        return true;
    default:
        return false;
    }
}

int32_t loader_subscribe_dispatch(sdk_bridge_session* session, plugin_context_t* ctx,
                                  const std::string& topic, bool once,
                                  cs_managed_sdk_call* call) {
    if (topic.empty() || call->callback == nullptr ||
        call->callback->kind != cs_managed_callback_kind::event) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    void* managed_token = nullptr;
    void* ignored_entry = nullptr;
    int32_t status =
        wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
    if (status != SAO_OK)
        return status;
    uint32_t token = 0;
    status = once ? loader::sao_plugins_ctx_subscribe_once(ctx, topic.c_str(),
                                                         callback_loader_event, managed_token,
                                                         &token)
                  : loader::sao_plugins_ctx_subscribe(ctx, topic.c_str(), callback_loader_event,
                                                    managed_token, &token);
    if (status != SAO_OK) {
        release_callback_token(managed_token);
        return status;
    }
    put_loader_binding(session, "sub:" + std::to_string(token), managed_token);
    if (call->out_callback_token != nullptr)
        *call->out_callback_token = managed_token;
    return write_call_json(ordered_json(token), call);
}

int32_t loader_timer_dispatch(sdk_bridge_session* session, plugin_context_t* ctx, double seconds,
                              bool one_shot, cs_managed_sdk_call* call) {
    if (call->callback == nullptr || call->callback->kind != cs_managed_callback_kind::timer ||
        !(seconds > 0.0) || !(seconds < 31536000.0)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    void* managed_token = nullptr;
    void* ignored_entry = nullptr;
    int32_t status =
        wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
    if (status != SAO_OK)
        return status;
    auto ud = std::make_unique<loader_timer_ud>();
    ud->managed_token = managed_token;
    ud->ctx = ctx;
    ud->one_shot = one_shot;
    char* raw_token = nullptr;
    status = one_shot ? loader::sao_plugins_ctx_set_timeout(ctx, callback_loader_timer, seconds,
                                                          ud.get(), &raw_token)
                      : loader::sao_plugins_ctx_set_interval(ctx, callback_loader_timer, seconds,
                                                             ud.get(), &raw_token);
    if (status != SAO_OK) {
        release_callback_token(managed_token);
        return status;
    }
    if (raw_token == nullptr || raw_token[0] == '\0') {
        if (raw_token != nullptr)
            loader::sao_plugins_ctx_free_string(raw_token);
        release_callback_token(managed_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    ud->token = raw_token;
    loader::sao_plugins_ctx_free_string(raw_token);
    ud->managed_id = fnv1a64(ud->token);
    const std::string token_string = ud->token;
    try {
        std::lock_guard lock(session->mutex);
        session->loader_bindings["timer:" + token_string] = managed_token;
        session->timer_ud_pool.push_back(std::move(ud));
    } catch (...) {
        (void)loader::sao_plugins_ctx_clear_timer(ctx, token_string.c_str());
        release_callback_token(managed_token);
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (call->out_callback_token != nullptr)
        *call->out_callback_token = managed_token;
    return write_call_json(ordered_json(token_string), call);
}

// Shared "register a v3 entity provider from already-wrapped managed tokens"
// used by the canonical register_menu_category / register_action_handler
// dispatches. Mirrors the table-slot flow (qualified id, dedup, teardown via
// session->entities) but lets the caller pick native snapshot/action entries —
// including the decline stub for snapshot-only menus.
int32_t register_entity_provider_v3_dispatch(
    sdk_bridge_session* session, const std::string& provider_id, void* snapshot_token,
    void* action_v2_token, const loader::entity_root_contribution_descriptor* root,
    uint32_t flags, std::string& out_qualified_id) noexcept {
    try {
        plugin_context_t* ctx = session->loader_context;
        entity_registration registration;
        registration.provider_id = provider_id;
        const char* plugin_id = loader::sao_plugins_ctx_plugin_id(ctx);
        if (plugin_id == nullptr || plugin_id[0] == '\0')
            return SAO_ERR_HANDLE_INVALID;
        registration.qualified_provider_id = std::string(plugin_id) + "/" + provider_id;
        registration.callbacks = std::make_unique<entity_callback_pair>();
        registration.callbacks->snapshot = snapshot_token;
        registration.callbacks->action_v2 = action_v2_token;
        {
            std::lock_guard lock(session->mutex);
            const auto duplicate =
                std::find_if(session->entities.begin(), session->entities.end(),
                             [&registration](const entity_registration& current) {
                                 return current.provider_id == registration.provider_id;
                             });
            if (duplicate != session->entities.end())
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            if (std::find(session->pending_entity_ids.begin(), session->pending_entity_ids.end(),
                          registration.provider_id) != session->pending_entity_ids.end()) {
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            session->entities.reserve(session->entities.size() +
                                      session->pending_entity_ids.size() + 1);
            session->pending_entity_ids.push_back(registration.provider_id);
        }
        const auto clear_pending = [&session, &registration]() noexcept {
            try {
                std::lock_guard lock(session->mutex);
                std::erase(session->pending_entity_ids, registration.provider_id);
            } catch (...) {
            }
        };
        const bool action_only = (flags & loader::kContextEntityProviderV3ActionOnly) != 0;
        loader::context_entity_provider_descriptor_v3 native{};
        native.struct_size = sizeof(native);
        native.provider_id_utf8 = registration.provider_id.c_str();
        // ACTION_ONLY descriptors require snapshot/user_data/root_contribution
        // to be null; the pair still reaches action invocations through
        // action_user_data.
        native.snapshot = !action_only && snapshot_token != nullptr
                              ? callback_entity_snapshot_v2
                              : nullptr;
        native.action_handler = nullptr;
        native.user_data = action_only ? nullptr : registration.callbacks.get();
        native.root_contribution = action_only ? nullptr : root;
        native.action_handler_v2 =
            action_v2_token != nullptr ? callback_entity_action_v2 : decline_entity_action_v2;
        native.action_user_data = registration.callbacks.get();
        native.flags = flags;
        native.reserved = 0;
        const int32_t status =
            loader::sao_plugins_ctx_register_entity_provider_v3(ctx, &native);
        if (status != SAO_OK) {
            clear_pending();
            return status;
        }
        {
            std::lock_guard lock(session->mutex);
            out_qualified_id = registration.qualified_provider_id;
            session->entities.push_back(std::move(registration));
            std::erase(session->pending_entity_ids, provider_id);
        }
        return SAO_OK;
    } catch (...) {
        try {
            std::lock_guard lock(session->mutex);
            std::erase(session->pending_entity_ids, provider_id);
        } catch (...) {
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t loader_menu_category_dispatch(sdk_bridge_session* session, plugin_context_t* ctx,
                                      const ordered_json& args,
                                      cs_managed_sdk_call* call) {
    const std::string name = arg_utf8(args, "name");
    if (name.empty())
        return SAO_ERR_INVALID_ARGUMENT;
    const std::string icon = arg_utf8(args, "icon");
    const double priority = arg_f64(args, "priority", 50.0);
    const char* plugin_id = loader::sao_plugins_ctx_plugin_id(ctx);
    if (plugin_id == nullptr || plugin_id[0] == '\0')
        return SAO_ERR_HANDLE_INVALID;
    // Canonical provider identity — fnv64 over the category name, matching the
    // python host's NativeMenuBridge hashing so menu toolchains behave alike.
    const std::string provider_id = "menu-" + hex64(fnv1a64(name));
    if (call->callback == nullptr) {
        const int32_t status = loader::sao_plugins_ctx_register_menu_category(
            ctx, name.c_str(), icon.c_str(), nullptr, static_cast<float>(priority), nullptr);
        if (status != SAO_OK)
            return status;
        ordered_json result;
        result["id"] = provider_id;
        return write_call_json(result, call);
    }
    if (call->callback->kind != cs_managed_callback_kind::entity_snapshot)
        return SAO_ERR_INVALID_ARGUMENT;
    void* managed_token = nullptr;
    void* ignored_entry = nullptr;
    int32_t status =
        wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
    if (status != SAO_OK)
        return status;
    const std::string contribution_id = provider_id;
    const std::string root_id =
        "plugin:" + hex64(fnv1a64(std::string(plugin_id) + "\n" + name));
    loader::entity_root_contribution_descriptor root{};
    root.struct_size = sizeof(root);
    root.contribution_id_utf8 = contribution_id.c_str();
    root.root_id_utf8 = root_id.c_str();
    root.name_utf8 = name.c_str();
    root.icon_utf8 = icon.c_str();
    root.priority = priority;
    std::string qualified_id;
    status = register_entity_provider_v3_dispatch(session, provider_id, managed_token, nullptr,
                                                  &root, 0, qualified_id);
    if (status != SAO_OK) {
        release_callback_token(managed_token);
        return status;
    }
    if (call->out_callback_token != nullptr)
        *call->out_callback_token = managed_token;
    ordered_json result;
    result["id"] = qualified_id;
    return write_call_json(result, call);
}

int32_t loader_action_handler_dispatch(sdk_bridge_session* session, plugin_context_t* ctx,
                                       cs_managed_sdk_call* call) {
    if (call->callback == nullptr ||
        call->callback->kind != cs_managed_callback_kind::entity_action_v2) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    // Canonical opaque action sink: ACTION_ONLY replace semantics keep
    // re-register idempotent (v3 replaces only the action binding when the
    // provider exists).
    constexpr const char* kOpaqueProviderId = "opaque-actions";
    entity_callback_pair* pair = nullptr;
    {
        std::lock_guard lock(session->mutex);
        const auto found = std::find_if(session->entities.begin(), session->entities.end(),
                                        [](const entity_registration& current) {
                                            return current.provider_id == kOpaqueProviderId;
                                        });
        if (found != session->entities.end())
            pair = found->callbacks.get();
    }
    void* managed_token = nullptr;
    void* ignored_entry = nullptr;
    int32_t status =
        wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
    if (status != SAO_OK)
        return status;
    if (pair != nullptr) {
        void* old_token = pair->action_v2;
        loader::context_entity_provider_descriptor_v3 native{};
        native.struct_size = sizeof(native);
        native.provider_id_utf8 = kOpaqueProviderId;
        native.snapshot = nullptr;
        native.action_handler = nullptr;
        native.user_data = nullptr;
        native.root_contribution = nullptr;
        native.action_handler_v2 = callback_entity_action_v2;
        native.action_user_data = pair;
        native.flags = loader::kContextEntityProviderV3ActionOnly;
        native.reserved = 0;
        status = loader::sao_plugins_ctx_register_entity_provider_v3(ctx, &native);
        if (status != SAO_OK) {
            release_callback_token(managed_token);
            return status;
        }
        pair->action_v2 = managed_token;
        if (old_token != nullptr && old_token != managed_token)
            release_callback_token(old_token);
        if (call->out_callback_token != nullptr)
            *call->out_callback_token = managed_token;
        const char* plugin_id = loader::sao_plugins_ctx_plugin_id(ctx);
        ordered_json result;
        result["id"] =
            std::string(plugin_id != nullptr ? plugin_id : "") + "/" + kOpaqueProviderId;
        return write_call_json(result, call);
    }
    std::string qualified_id;
    status = register_entity_provider_v3_dispatch(session, kOpaqueProviderId, nullptr,
                                                  managed_token, nullptr,
                                                  loader::kContextEntityProviderV3ActionOnly,
                                                  qualified_id);
    if (status != SAO_OK) {
        release_callback_token(managed_token);
        return status;
    }
    if (call->out_callback_token != nullptr)
        *call->out_callback_token = managed_token;
    ordered_json result;
    result["id"] = qualified_id;
    return write_call_json(result, call);
}

int32_t dispatch_loader_method(sdk_bridge_session* session, sdk_method_id method,
                               const ordered_json& args, cs_managed_sdk_call* call) noexcept {
    try {
        plugin_context_t* ctx = session->loader_context;
        switch (method) {
        case sdk_method_id::prop_should_stop:
            return write_call_json(ordered_json(loader::sao_plugins_ctx_should_stop(ctx)), call);

        case sdk_method_id::method_subscribe:
        case sdk_method_id::method_subscribe_once: {
            const std::string topic = arg_utf8(args, "topic");
            return loader_subscribe_dispatch(session, ctx, topic,
                                             method == sdk_method_id::method_subscribe_once, call);
        }

        case sdk_method_id::method_on_damage:
        case sdk_method_id::method_on_heal:
        case sdk_method_id::method_on_skill:
        case sdk_method_id::method_on_boss:
        case sdk_method_id::method_on_snapshot:
        case sdk_method_id::method_on_encounter_finalized:
            return loader_subscribe_dispatch(session, ctx, fixed_event_topic(method), false, call);

        case sdk_method_id::method_unsubscribe: {
            uint32_t token = 0;
            const ordered_json* value = arg_member(args, "token");
            if (value != nullptr && value->is_number()) {
                token = static_cast<uint32_t>(value->get<uint64_t>());
            } else if (value != nullptr && value->is_string()) {
                token = static_cast<uint32_t>(
                    std::strtoul(value->get<std::string>().c_str(), nullptr, 10));
            } else {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            const int32_t status = loader::sao_plugins_ctx_unsubscribe(ctx, token);
            if (status != SAO_OK)
                return status;
            release_loader_binding(session, "sub:" + std::to_string(token));
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_emit: {
            const std::string topic = arg_utf8(args, "topic");
            if (topic.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const ordered_json* payload = arg_member(args, "payload");
            // The loader wraps payload in the {topic,payload} envelope and
            // requires parseable JSON — dump() keeps any managed value shape
            // valid (strings, objects, scalars all remain legal JSON).
            const std::string payload_json =
                payload == nullptr ? "{}" : payload->dump();
            const int32_t status =
                loader::sao_plugins_ctx_emit(ctx, topic.c_str(), payload_json.c_str());
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_get_snapshot: {
            char* raw = nullptr;
            const int32_t status = loader::sao_plugins_ctx_get_snapshot(ctx, &raw);
            if (status != SAO_OK)
                return status;
            if (raw == nullptr)
                return write_call_json(ordered_json::object(), call);
            const size_t size = std::strlen(raw);
            const int32_t written =
                write_raw_result(raw, size, call->out_result_json_utf8, call->out_capacity,
                                 call->out_required);
            loader::sao_plugins_ctx_free_string(raw);
            return written;
        }

        case sdk_method_id::method_snapshot_value: {
            const std::string path = arg_utf8(args, "path");
            if (path.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            char* raw = nullptr;
            int32_t status = loader::sao_plugins_ctx_get_snapshot(ctx, &raw);
            if (status != SAO_OK)
                return status;
            ordered_json snapshot;
            try {
                snapshot = raw != nullptr ? ordered_json::parse(raw) : ordered_json::object();
            } catch (...) {
                snapshot = ordered_json::object();
            }
            if (raw != nullptr)
                loader::sao_plugins_ctx_free_string(raw);
            // Canonical dotted-path navigation across the snapshot map.
            const ordered_json* cursor = &snapshot;
            bool found = true;
            size_t head = 0;
            while (found && head <= path.size()) {
                const size_t dot = path.find('.', head);
                const std::string segment = path.substr(
                    head, dot == std::string::npos ? std::string::npos : dot - head);
                if (cursor->is_object() && cursor->contains(segment)) {
                    cursor = &(*cursor)[segment];
                } else {
                    found = false;
                }
                if (dot == std::string::npos)
                    break;
                head = dot + 1;
            }
            if (found)
                return write_call_json(*cursor, call);
            const ordered_json* fallback = arg_member(args, "default");
            return write_call_json(fallback == nullptr ? ordered_json(nullptr) : *fallback, call);
        }

        case sdk_method_id::method_recent_events: {
            const uint32_t limit = arg_u32(args, "limit", 20);
            const std::string topic = arg_utf8(args, "topic");
            char* raw = nullptr;
            const int32_t status = loader::sao_plugins_ctx_recent_events(
                ctx, limit, topic.empty() ? nullptr : topic.c_str(), &raw);
            if (status != SAO_OK)
                return status;
            if (raw == nullptr)
                return write_call_json(ordered_json::array(), call);
            const size_t size = std::strlen(raw);
            const int32_t written =
                write_raw_result(raw, size, call->out_result_json_utf8, call->out_capacity,
                                 call->out_required);
            loader::sao_plugins_ctx_free_string(raw);
            return written;
        }

        case sdk_method_id::method_register_parser_adapter:
        case sdk_method_id::method_register_exporter:
        case sdk_method_id::method_register_formatter:
        case sdk_method_id::method_register_trigger_type:
        case sdk_method_id::method_register_report_view:
        case sdk_method_id::method_register_timer: {
            const char* kind = extension_kind_for(method);
            const std::string id = arg_utf8(args, "id");
            if (kind == nullptr || id.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const ordered_json* meta = arg_member(args, "meta");
            const std::string meta_json =
                meta == nullptr || meta->is_null() ? std::string() : meta->dump();
            const int32_t status = loader::sao_plugins_ctx_register_extension(
                ctx, kind, id.c_str(), meta_json.empty() ? nullptr : meta_json.c_str(), nullptr,
                nullptr);
            if (status != SAO_OK)
                return status;
            ordered_json result;
            result["id"] = id;
            result["kind"] = kind;
            return write_call_json(result, call);
        }

        case sdk_method_id::method_register_ui_panel: {
            if (call->callback != nullptr && !valid_callback_descriptor(call->callback))
                return SAO_ERR_INVALID_ARGUMENT;
            if (call->callback == nullptr ||
                call->callback->kind != cs_managed_callback_kind::render_hook)
                return kDispatchFallthrough;
            const std::string id = arg_utf8(args, "id");
            const auto* metadata = arg_member(args, "metadata");
            if (id.empty() || metadata == nullptr || !metadata->is_object())
                return SAO_ERR_INVALID_ARGUMENT;
            const std::string serialized = metadata->dump();
            auto panel = std::make_unique<loader_panel_ud>();
            panel->surface = id;
            auto* payload = panel.get();
            {
                std::lock_guard lock(session->mutex);
                for (const auto& existing : session->panel_ud_pool)
                    if (existing->surface == id)
                        return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
                session->panel_ud_pool.push_back(std::move(panel));
            }
            const auto release_panel = [&] {
                release_callback_token(payload->action_token);
                release_callback_token(payload->render_token);
                std::lock_guard lock(session->mutex);
                std::erase_if(session->panel_ud_pool,
                              [payload](const auto& entry) { return entry.get() == payload; });
            };
            void* ignored_entry = nullptr;
            int32_t status = wrap_managed_callback(
                session, call->callback, &ignored_entry, &payload->render_token);
            if (status != SAO_OK) {
                release_panel();
                return status;
            }
            const bool has_action = arg_bool(args, "has_action", false);
            if (has_action) {
                auto action_descriptor = *call->callback;
                action_descriptor.kind = cs_managed_callback_kind::panel_action;
                status = wrap_managed_callback(
                    session, &action_descriptor, &ignored_entry, &payload->action_token);
                if (status != SAO_OK) {
                    release_panel();
                    return status;
                }
            }
            status = loader::sao_plugins_ctx_register_ui_panel(
                ctx, id.c_str(), serialized.c_str(), callback_loader_panel_render,
                has_action ? callback_loader_panel_action : nullptr, payload);
            if (status != SAO_OK) {
                const int32_t cleanup = status == loader::SAO_PLUGINS_ERR_ALREADY_EXISTS ?
                    SAO_OK : loader::sao_plugins_ctx_unregister_ui_panel(ctx, id.c_str());
                if (cleanup == SAO_OK || cleanup == SAO_ERR_HANDLE_INVALID)
                    release_panel();
                return status;
            }
            if (call->out_callback_token != nullptr)
                *call->out_callback_token = payload->render_token;
            return write_call_json(ordered_json{{"id", id}}, call);
        }

        case sdk_method_id::method_register_render_hook: {
            const std::string surface = arg_utf8(args, "surface");
            if (surface.empty() || call->callback == nullptr ||
                call->callback->kind != cs_managed_callback_kind::render_hook) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            const float priority = static_cast<float>(arg_f64(args, "priority", 0.0));
            void* managed_token = nullptr;
            void* ignored_entry = nullptr;
            int32_t status =
                wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
            if (status != SAO_OK)
                return status;
            uint32_t token = 0;
            status = loader::sao_plugins_ctx_register_render_hook(
                ctx, surface.c_str(), priority, callback_render_hook, managed_token, &token);
            if (status != SAO_OK) {
                release_callback_token(managed_token);
                return status;
            }
            put_loader_binding(session, "hook:" + std::to_string(token), managed_token);
            if (call->out_callback_token != nullptr)
                *call->out_callback_token = managed_token;
            return write_call_json(ordered_json(token), call);
        }

        case sdk_method_id::method_request_redraw: {
            const std::string surface = arg_utf8(args, "surface");
            const std::string reason = arg_utf8(args, "reason");
            if (surface.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_request_redraw(
                ctx, surface.c_str(), reason.c_str());
            return status == SAO_OK ? write_call_json(ordered_json(true), call) : status;
        }

        case sdk_method_id::method_register_data_source: {
            const std::string id = arg_utf8(args, "id");
            const std::string alt_id = arg_utf8(args, "source_id");
            const std::string source_id = !id.empty() ? id : alt_id;
            if (source_id.empty() || call->callback == nullptr ||
                call->callback->kind != cs_managed_callback_kind::data_source) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            const ordered_json* meta = arg_member(args, "meta");
            const std::string meta_json =
                meta == nullptr || meta->is_null() ? std::string() : meta->dump();
            void* managed_token = nullptr;
            void* ignored_entry = nullptr;
            int32_t status =
                wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
            if (status != SAO_OK)
                return status;
            status = loader::sao_plugins_ctx_register_data_source(
                ctx, source_id.c_str(), meta_json.empty() ? nullptr : meta_json.c_str(),
                callback_data_source_start, callback_data_source_stop, managed_token);
            if (status != SAO_OK) {
                release_callback_token(managed_token);
                return status;
            }
            put_loader_binding(session, "dsrc:" + source_id, managed_token);
            if (call->out_callback_token != nullptr)
                *call->out_callback_token = managed_token;
            ordered_json result;
            result["id"] = source_id;
            return write_call_json(result, call);
        }

        case sdk_method_id::method_register_menu_category:
            return loader_menu_category_dispatch(session, ctx, args, call);

        case sdk_method_id::method_register_menu_surface: {
            const std::string surface_id = arg_utf8(args, "surface_id");
            const std::string alt_id = arg_utf8(args, "id");
            const std::string id = !surface_id.empty() ? surface_id : alt_id;
            if (id.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const ordered_json* descriptor = arg_member(args, "descriptor");
            const std::string descriptor_json =
                descriptor == nullptr || descriptor->is_null() ? "{}" : descriptor->dump();
            const float priority = static_cast<float>(arg_f64(args, "priority", 0.0));
            const int32_t status = loader::sao_plugins_ctx_register_menu_surface(
                ctx, id.c_str(), descriptor_json.c_str(), priority);
            if (status != SAO_OK)
                return status;
            ordered_json result;
            result["id"] = id;
            return write_call_json(result, call);
        }

        case sdk_method_id::method_register_action_handler:
            return loader_action_handler_dispatch(session, ctx, call);

        case sdk_method_id::method_set_interval:
        case sdk_method_id::method_set_timeout: {
            const double seconds = arg_f64(args, "seconds", arg_f64(args, "interval", 0.0));
            return loader_timer_dispatch(session, ctx, seconds,
                                         method == sdk_method_id::method_set_timeout, call);
        }

        case sdk_method_id::method_clear_timer: {
            const std::string token = arg_utf8(args, "token");
            if (token.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_clear_timer(ctx, token.c_str());
            if (status != SAO_OK)
                return status;
            release_loader_binding(session, "timer:" + token);
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_notify: {
            const std::string title = arg_utf8(args, "title");
            std::string message = arg_utf8(args, "message");
            if (message.empty())
                message = arg_utf8(args, "text");
            const double duration = arg_f64(args, "duration_s", arg_f64(args, "duration", 3.0));
            const std::string kind = arg_utf8(args, "kind", "info");
            const int32_t status = loader::sao_plugins_ctx_notify(
                ctx, title.c_str(), message.c_str(), duration,
                kind.empty() ? "info" : kind.c_str());
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_dismiss_notify: {
            const int32_t status = loader::sao_plugins_ctx_dismiss_notify(ctx);
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_toast: {
            std::string message = arg_utf8(args, "message");
            if (message.empty())
                message = arg_utf8(args, "text");
            if (message.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_toast(ctx, message.c_str());
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_open_file: {
            const ordered_json* filters = arg_member(args, "filters");
            std::string filters_json;
            if (filters != nullptr && !filters->is_null()) {
                filters_json = filters->is_string() ? filters->get<std::string>()
                                                    : filters->dump();
            }
            const std::string title = arg_utf8(args, "title");
            const std::string initial_dir = arg_utf8(args, "initial_dir");
            const int64_t hwnd = arg_i64(args, "hwnd", 0);
            const std::wstring initial_dir_w =
                initial_dir.empty() ? std::wstring() : wide_from_utf8(initial_dir);
            wchar_t* selected = nullptr;
            const int32_t status = loader::sao_plugins_ctx_open_file(
                ctx, filters_json.empty() ? nullptr : filters_json.c_str(),
                title.empty() ? nullptr : title.c_str(),
                initial_dir_w.empty() ? nullptr : initial_dir_w.c_str(),
                static_cast<intptr_t>(hwnd), &selected);
            if (status != SAO_OK)
                return status;
            if (selected == nullptr)
                return write_call_json(ordered_json(nullptr), call);
            const std::string utf8_path = utf8_from_wide(selected);
            loader::sao_plugins_ctx_free_wstring(selected);
            return write_call_json(ordered_json(utf8_path), call);
        }

        case sdk_method_id::method_open_window: {
            const std::string panel_id = arg_utf8(args, "panel_id");
            const uint32_t width = arg_u32(args, "width", 0);
            const uint32_t height = arg_u32(args, "height", 0);
            const int32_t status = loader::sao_plugins_ctx_open_window(
                ctx, panel_id.empty() ? nullptr : panel_id.c_str(), width, height);
            if (status != SAO_OK)
                return status;
            ordered_json result;
            result["panel_id"] = panel_id;
            return write_call_json(result, call);
        }

        case sdk_method_id::method_create_compositor_layer: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const uint32_t width = arg_u32(args, "width", 0);
            const uint32_t height = arg_u32(args, "height", 0);
            const int32_t x = static_cast<int32_t>(arg_i64(args, "x", 0));
            const int32_t y = static_cast<int32_t>(arg_i64(args, "y", 0));
            const int32_t z = static_cast<int32_t>(arg_i64(args, "z", 140));
            const bool click_through = arg_bool(args, "click_through", true);
            const bool high_fps = arg_bool(args, "high_fps", false);
            const uint32_t target_fps = arg_u32(args, "target_fps", 0);
            const int32_t status = loader::sao_plugins_ctx_create_compositor_layer(
                ctx, name.c_str(), width, height, x, y, z, click_through, high_fps, target_fps);
            if (status != SAO_OK)
                return status;
            ordered_json result;
            result["name"] = name;
            return write_call_json(result, call);
        }

        case sdk_method_id::method_upload_compositor_frame: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            std::string encoded = arg_utf8(args, "data_base64");
            if (encoded.empty())
                encoded = arg_utf8(args, "data");
            const uint32_t width = arg_u32(args, "width", 0);
            const uint32_t height = arg_u32(args, "height", 0);
            std::vector<uint8_t> bytes;
            if (encoded.empty() || !base64_decode(encoded, bytes) || bytes.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_upload_compositor_frame(
                ctx, name.c_str(), bytes.data(), bytes.size(), width, height);
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_set_compositor_layer_position: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_set_compositor_layer_position(
                ctx, name.c_str(), static_cast<int32_t>(arg_i64(args, "x", 0)),
                static_cast<int32_t>(arg_i64(args, "y", 0)));
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_set_compositor_layer_visible: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_set_compositor_layer_visible(
                ctx, name.c_str(), arg_bool(args, "visible", true));
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_destroy_compositor_layer: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status =
                loader::sao_plugins_ctx_destroy_compositor_layer(ctx, name.c_str());
            if (status != SAO_OK)
                return status;
            return write_call_json(ordered_json(true), call);
        }

        case sdk_method_id::method_set_compositor_layer_input: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty() || call->callback == nullptr ||
                call->callback->kind != cs_managed_callback_kind::compositor_input) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            void* managed_token = nullptr;
            void* ignored_entry = nullptr;
            int32_t status =
                wrap_managed_callback(session, call->callback, &ignored_entry, &managed_token);
            if (status != SAO_OK)
                return status;
            status = loader::sao_plugins_ctx_set_compositor_layer_input(
                ctx, name.c_str(), callback_compositor_cursor_pos,
                callback_compositor_mouse_button, callback_compositor_cursor_leave,
                callback_compositor_scroll, managed_token);
            if (status != SAO_OK) {
                release_callback_token(managed_token);
                return status;
            }
            put_loader_binding(session, "ci:" + name, managed_token);
            if (call->out_callback_token != nullptr)
                *call->out_callback_token = managed_token;
            ordered_json result;
            result["name"] = name;
            return write_call_json(result, call);
        }

        case sdk_method_id::method_get_engine: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            void* engine = loader::sao_plugins_ctx_get_engine(ctx, name.c_str());
            ordered_json result;
            result["handle"] =
                engine == nullptr
                    ? ordered_json(nullptr)
                    : ordered_json(reinterpret_cast<uintptr_t>(engine));
            return write_call_json(result, call);
        }

        case sdk_method_id::method_require_engine: {
            const std::string name = arg_utf8(args, "name");
            if (name.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            void* engine = loader::sao_plugins_ctx_get_engine(ctx, name.c_str());
            if (engine == nullptr)
                return SAO_ERR_HANDLE_INVALID;
            ordered_json result;
            result["handle"] = reinterpret_cast<uintptr_t>(engine);
            return write_call_json(result, call);
        }

        case sdk_method_id::method_ensure_requirements: {
            const bool install = arg_bool(args, "install", true);
            char* raw = nullptr;
            const int32_t status =
                loader::sao_plugins_ctx_ensure_requirements(ctx, install, &raw);
            if (status != SAO_OK)
                return status;
            if (raw == nullptr)
                return write_call_json(ordered_json::object(), call);
            const size_t size = std::strlen(raw);
            const int32_t written =
                write_raw_result(raw, size, call->out_result_json_utf8, call->out_capacity,
                                 call->out_required);
            loader::sao_plugins_ctx_free_string(raw);
            return written;
        }

        case sdk_method_id::method_load_local: {
            std::string rel = arg_utf8(args, "path");
            if (rel.empty())
                rel = arg_utf8(args, "rel");
            if (rel.empty())
                return SAO_ERR_INVALID_ARGUMENT;
            const char* plugin_id = loader::sao_plugins_ctx_plugin_id(ctx);
            const wchar_t* root_dir = loader::sao_plugins_ctx_path(ctx);
            if (plugin_id == nullptr || plugin_id[0] == '\0' || root_dir == nullptr)
                return SAO_ERR_HANDLE_INVALID;
            script_ctx::load_local_result kind = script_ctx::load_local_result::missing;
            std::shared_ptr<script_ctx::script_module> module;
            std::wstring abs_path;
            std::string diag;
            const int32_t status = script_ctx::runtime_bridge_load_local(
                ctx, plugin_id, root_dir, rel.c_str(), &kind, &module, &abs_path, &diag);
            if (status != SAO_OK)
                return status;
            ordered_json result;
            switch (kind) {
            case script_ctx::load_local_result::module:
                result["kind"] = "module";
                result["path"] = utf8_from_wide(abs_path);
                result["module_id"] = module != nullptr ? module->module_id() : "";
                result["diag"] = nullptr;
                break;
            case script_ctx::load_local_result::path_only:
                result["kind"] = "path";
                result["path"] = utf8_from_wide(abs_path);
                result["module_id"] = nullptr;
                result["diag"] = diag.empty() ? ordered_json(nullptr) : ordered_json(diag);
                break;
            case script_ctx::load_local_result::missing:
                result["kind"] = "missing";
                result["path"] = nullptr;
                result["module_id"] = nullptr;
                result["diag"] = diag.empty() ? ordered_json(nullptr) : ordered_json(diag);
                break;
            case script_ctx::load_local_result::unsupported:
            default:
                result["kind"] = "unsupported";
                result["path"] = nullptr;
                result["module_id"] = nullptr;
                result["diag"] = diag.empty() ? ordered_json(nullptr) : ordered_json(diag);
                break;
            }
            return write_call_json(result, call);
        }

        case sdk_method_id::method_compositor_display_refresh_hz: {
#if defined(_WIN32)
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            using enum_display_settings_fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
            const auto query = user32 == nullptr ? nullptr :
                reinterpret_cast<enum_display_settings_fn>(GetProcAddress(user32, "EnumDisplaySettingsW"));
            if (query == nullptr)
                return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
            DEVMODEW mode{};
            mode.dmSize = static_cast<WORD>(sizeof(mode));
            if (!query(nullptr, ENUM_CURRENT_SETTINGS, &mode))
                return SAO_ERR_OS_CALL_FAILED;
            if ((mode.dmFields & DM_DISPLAYFREQUENCY) == 0 || mode.dmDisplayFrequency <= 1)
                return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
            return write_call_json(ordered_json(mode.dmDisplayFrequency), call);
#else
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
#endif
        }

        case sdk_method_id::method_set_compositor_layer_mmf_source: {
            const auto* name = arg_member(args, "name");
            const auto* mmf = arg_member(args, "mmf_name");
            if (!name || !name->is_string() || !mmf || !mmf->is_string())
                return SAO_ERR_INVALID_ARGUMENT;
            const auto layer = name->get<std::string>();
            const auto source = mmf->get<std::string>();
            if (layer.empty() || layer.find('\0') != std::string::npos ||
                source.find('\0') != std::string::npos)
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_set_compositor_layer_mmf_source(
                ctx, layer.c_str(), source.c_str());
            return status == SAO_OK ? write_call_json(true, call) : status;
        }
        case sdk_method_id::method_set_compositor_layer_shared_texture_source: {
            const auto* name = arg_member(args, "name");
            if (!name || !name->is_string()) return SAO_ERR_INVALID_ARGUMENT;
            const auto layer = name->get<std::string>();
            if (layer.empty() || layer.find('\0') != std::string::npos)
                return SAO_ERR_INVALID_ARGUMENT;
            const auto integer = [&](const char* key, uint64_t maximum, uint64_t& out) {
                const auto* value = arg_member(args, key);
                if (!value || !value->is_number_integer()) return false;
                if (value->is_number_unsigned()) out = value->get<uint64_t>();
                else {
                    const auto signed_value = value->get<int64_t>();
                    if (signed_value < 0) return false;
                    out = static_cast<uint64_t>(signed_value);
                }
                return out <= maximum;
            };
            uint64_t handle = 0, width = 0, height = 0;
            if (!integer("handle", UINT64_MAX, handle) ||
                !integer("width", UINT32_MAX, width) || !integer("height", UINT32_MAX, height))
                return SAO_ERR_INVALID_ARGUMENT;
            const int32_t status = loader::sao_plugins_ctx_set_compositor_layer_shared_texture_source(
                ctx, layer.c_str(), handle, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            return status == SAO_OK ? write_call_json(true, call) : status;
        }
        case sdk_method_id::method_compositor_gpu_interop_available: {
            bool available = false;
            const int32_t status = loader::sao_plugins_ctx_compositor_gpu_interop_available(ctx, &available);
            return status == SAO_OK ? write_call_json(available, call) : status;
        }
        case sdk_method_id::method_compositor_layer_shared_texture_active: {
            const auto* name = arg_member(args, "name");
            if (!name || !name->is_string()) return SAO_ERR_INVALID_ARGUMENT;
            const auto layer = name->get<std::string>();
            if (layer.empty() || layer.find('\0') != std::string::npos)
                return SAO_ERR_INVALID_ARGUMENT;
            bool active = false;
            const int32_t status = loader::sao_plugins_ctx_compositor_layer_shared_texture_active(
                ctx, layer.c_str(), &active);
            return status == SAO_OK ? write_call_json(active, call) : status;
        }

        case sdk_method_id::method_run_on_ui:
        case sdk_method_id::method_register_engine:
        case sdk_method_id::method_call_engine:
        case sdk_method_id::method_call_runtime:
            return loader::SAO_PLUGINS_ERR_UNSUPPORTED;

        default:
            return kDispatchFallthrough;
        }
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL table_dispatch(cs_managed_sdk_session_t opaque,
                                        cs_managed_sdk_call* call) {
    if (call == nullptr || call->struct_size < sizeof(cs_managed_sdk_call) ||
        (call->args_size != 0 && call->args_json_utf8 == nullptr) ||
        call->method_id >= static_cast<uint16_t>(sdk_method_id::method_count_)) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (call->out_callback_token != nullptr)
        *call->out_callback_token = nullptr;
    session_call_lease lease;
    int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    auto* session = lease.get();

    // Canonical ctx expansion: loader-context-owned methods run first so the
    // event/timer/dialog/compositor domains route to the canonical
    // plugin_context_t ABI; SDK-domain ids fall through to the existing
    // sdk_context dispatch unchanged.
    if (session->loader_context != nullptr &&
        loader_ctx_owns_method(static_cast<sdk_method_id>(call->method_id))) {
        ordered_json loader_args;
        if (!parse_call_args(call, loader_args)) {
            remember_error(session, SAO_ERR_INVALID_ARGUMENT, "loader dispatch");
            return SAO_ERR_INVALID_ARGUMENT;
        }
        status = dispatch_loader_method(session, static_cast<sdk_method_id>(call->method_id),
                                        loader_args, call);
        if (status != kDispatchFallthrough) {
            remember_error(session, status, "loader dispatch");
            return status;
        }
    }

    if (session->sdk_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;

    sdk_context_call_request request{};
    request.args_json_utf8 = call->args_json_utf8;
    request.args_size = call->args_size;
    request.out_result_json_utf8 = call->out_result_json_utf8;
    request.out_capacity = call->out_capacity;
    request.out_required = call->out_required;

    void* callback_token = nullptr;
    void* entry = nullptr;
    if (call->callback != nullptr) {
        status = wrap_managed_callback(session, call->callback, &entry, &callback_token);
        if (status != SAO_OK)
            return status;
        request.callback_user_data = callback_token;
        switch (call->callback->kind) {
        case cs_managed_callback_kind::event:
            request.event_callback =
                reinterpret_cast<sdk_binding::sdk_context_event_callback_fn>(entry);
            break;
        case cs_managed_callback_kind::hotkey:
            request.hotkey_callback =
                reinterpret_cast<sdk_binding::sdk_context_hotkey_callback_fn>(entry);
            break;
        case cs_managed_callback_kind::timer:
            request.timer_callback =
                reinterpret_cast<sdk_binding::sdk_context_timer_callback_fn>(entry);
            break;
        case cs_managed_callback_kind::panel_action:
            request.panel_action_callback =
                reinterpret_cast<sdk_binding::sdk_context_panel_action_callback_fn>(entry);
            break;
        default:
            release_callback_token(callback_token);
            return SAO_ERR_INVALID_ARGUMENT;
        }
    }

    status = sdk_binding::sao_plugins_sdk_context_dispatch(
        session->sdk_context, static_cast<sdk_method_id>(call->method_id), &request);
    if (status != SAO_OK && callback_token != nullptr) {
        release_callback_token(callback_token);
    } else if (status == SAO_OK && callback_token != nullptr &&
               call->out_callback_token != nullptr) {
        *call->out_callback_token = callback_token;
    }
    remember_error(session, status, "SDK dispatch");
    return status;
}

int32_t SAO_PLUGINS_CALL table_release_callback(cs_managed_sdk_session_t opaque,
                                                cs_managed_callback_token_t token) {
    if (token == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    auto* session = lease.get();
    {
        std::lock_guard lock(session->mutex);
        const auto found =
            std::find_if(session->callbacks.begin(), session->callbacks.end(),
                         [token](const managed_callback* callback) {
                             return callback->token == reinterpret_cast<uintptr_t>(token);
                         });
        if (found == session->callbacks.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        session->callback_release_status = SAO_OK;
    }
    release_callback_token(token);
    std::lock_guard lock(session->mutex);
    status = session->callback_release_status;
    if (status == SAO_OK && std::any_of(session->callbacks.begin(), session->callbacks.end(),
                                        [token](const managed_callback* callback) {
                                            return callback->token ==
                                                   reinterpret_cast<uintptr_t>(token);
                                        })) {
        status = loader::SAO_PLUGINS_ERR_BUSY;
    }
    return status;
}

int32_t SAO_PLUGINS_CALL table_log(cs_managed_sdk_session_t opaque, const char* message_utf8) {
    if (message_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    const int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    if (lease.get()->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    loader::sao_plugins_ctx_log(lease.get()->loader_context, message_utf8);
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL table_register_engine(cs_managed_sdk_session_t opaque,
                                               const char* name_utf8, void* engine) {
    if (name_utf8 == nullptr || engine == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    const int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    if (lease.get()->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    return loader::sao_plugins_ctx_register_engine(lease.get()->loader_context, name_utf8, engine);
}

int32_t SAO_PLUGINS_CALL table_get_engine(cs_managed_sdk_session_t opaque, const char* name_utf8,
                                          void** out_engine) {
    if (name_utf8 == nullptr || out_engine == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_engine = nullptr;
    session_call_lease lease;
    const int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    if (lease.get()->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    *out_engine = loader::sao_plugins_ctx_get_engine(lease.get()->loader_context, name_utf8);
    return *out_engine == nullptr ? SAO_ERR_HANDLE_INVALID : SAO_OK;
}

int32_t SAO_PLUGINS_CALL table_register_entity_provider(
    cs_managed_sdk_session_t opaque, const cs_managed_entity_provider_descriptor* descriptor) {
    if (descriptor == nullptr ||
        descriptor->struct_size < sizeof(cs_managed_entity_provider_descriptor) ||
        descriptor->provider_id_utf8 == nullptr || descriptor->provider_id_utf8[0] == '\0' ||
        descriptor->snapshot == nullptr || descriptor->action_handler == nullptr ||
        descriptor->snapshot->kind != cs_managed_callback_kind::entity_snapshot ||
        descriptor->action_handler->kind != cs_managed_callback_kind::entity_action) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    session_call_lease lease;
    int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    auto* session = lease.get();
    if (session->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;

    try {
        entity_registration registration;
        registration.provider_id = descriptor->provider_id_utf8;
        const char* plugin_id = loader::sao_plugins_ctx_plugin_id(session->loader_context);
        if (plugin_id == nullptr || plugin_id[0] == '\0')
            return SAO_ERR_HANDLE_INVALID;
        registration.qualified_provider_id =
            std::string(plugin_id) + "/" + registration.provider_id;
        registration.callbacks = std::make_unique<entity_callback_pair>();
        {
            std::lock_guard lock(session->mutex);
            const auto duplicate =
                std::find_if(session->entities.begin(), session->entities.end(),
                             [&registration](const entity_registration& current) {
                                 return current.provider_id == registration.provider_id;
                             });
            if (duplicate != session->entities.end())
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            if (std::find(session->pending_entity_ids.begin(), session->pending_entity_ids.end(),
                          registration.provider_id) != session->pending_entity_ids.end()) {
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            session->entities.reserve(session->entities.size() +
                                      session->pending_entity_ids.size() + 1);
            session->pending_entity_ids.push_back(registration.provider_id);
        }

        const auto clear_pending = [&session, &registration]() noexcept {
            try {
                std::lock_guard lock(session->mutex);
                std::erase(session->pending_entity_ids, registration.provider_id);
            } catch (...) {
            }
        };

        void* ignored_entry = nullptr;
        status = wrap_managed_callback(session, descriptor->snapshot, &ignored_entry,
                                       &registration.callbacks->snapshot);
        if (status != SAO_OK) {
            clear_pending();
            return status;
        }
        status = wrap_managed_callback(session, descriptor->action_handler, &ignored_entry,
                                       &registration.callbacks->action);
        if (status != SAO_OK) {
            release_callback_token(registration.callbacks->snapshot);
            clear_pending();
            return status;
        }

        loader::entity_root_contribution_descriptor root{};
        const loader::entity_root_contribution_descriptor* root_ptr = nullptr;
        if (descriptor->contribution_id_utf8 != nullptr &&
            descriptor->contribution_id_utf8[0] != '\0') {
            if (descriptor->root_id_utf8 == nullptr || descriptor->name_utf8 == nullptr) {
                release_callback_token(registration.callbacks->action);
                release_callback_token(registration.callbacks->snapshot);
                clear_pending();
                return SAO_ERR_INVALID_ARGUMENT;
            }
            root = {sizeof(root),
                    descriptor->contribution_id_utf8,
                    descriptor->root_id_utf8,
                    descriptor->name_utf8,
                    descriptor->icon_utf8,
                    descriptor->priority};
            root_ptr = &root;
        }
        loader::context_entity_provider_descriptor native{};
        native.struct_size = sizeof(native);
        native.provider_id_utf8 = registration.provider_id.c_str();
        native.snapshot = callback_entity_snapshot;
        native.action_handler = callback_entity_action;
        native.user_data = registration.callbacks.get();
        native.root_contribution = root_ptr;
        status = loader::sao_plugins_ctx_register_entity_provider(session->loader_context, &native);
        if (status != SAO_OK) {
            release_callback_token(registration.callbacks->action);
            release_callback_token(registration.callbacks->snapshot);
            clear_pending();
            return status;
        }
        {
            std::lock_guard lock(session->mutex);
            session->entities.push_back(std::move(registration));
            std::erase(session->pending_entity_ids, descriptor->provider_id_utf8);
        }
        return SAO_OK;
    } catch (...) {
        try {
            std::lock_guard lock(session->mutex);
            std::erase(session->pending_entity_ids, descriptor->provider_id_utf8);
        } catch (...) {
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL table_unregister_entity_provider(cs_managed_sdk_session_t opaque,
                                                          const char* provider_id_utf8) {
    if (provider_id_utf8 == nullptr || provider_id_utf8[0] == '\0')
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    auto* session = lease.get();
    if (session->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;

    entity_callback_pair* callbacks = nullptr;
    std::string qualified_provider_id;
    try {
        std::lock_guard lock(session->mutex);
        const auto found = std::find_if(session->entities.begin(), session->entities.end(),
                                        [provider_id_utf8](const entity_registration& current) {
                                            return current.provider_id == provider_id_utf8;
                                        });
        if (found == session->entities.end())
            return SAO_ERR_HANDLE_INVALID;
        callbacks = found->callbacks.get();
        qualified_provider_id = found->qualified_provider_id;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }

    const char* ids[] = {qualified_provider_id.c_str()};
    status = loader::plugin_context_unregister_entity_providers(session->loader_context, ids, 1);
    if (status != SAO_OK)
        return status;

    std::unique_ptr<entity_callback_pair> owned_callbacks;
    {
        std::lock_guard lock(session->mutex);
        const auto found = std::find_if(session->entities.begin(), session->entities.end(),
                                        [callbacks](const entity_registration& current) {
                                            return current.callbacks.get() == callbacks;
                                        });
        if (found != session->entities.end()) {
            owned_callbacks = std::move(found->callbacks);
            session->entities.erase(found);
        }
    }
    if (owned_callbacks != nullptr) {
        if (owned_callbacks->action_v2 != nullptr)
            release_callback_token(owned_callbacks->action_v2);
        if (owned_callbacks->action != nullptr)
            release_callback_token(owned_callbacks->action);
        release_callback_token(owned_callbacks->snapshot);
    }
    return SAO_OK;
}

int32_t SAO_PLUGINS_CALL table_register_entity_provider_v2(
    cs_managed_sdk_session_t opaque, const cs_managed_entity_provider_descriptor_v2* descriptor) {
    if (descriptor == nullptr ||
        descriptor->struct_size < sizeof(cs_managed_entity_provider_descriptor_v2) ||
        descriptor->provider_id_utf8 == nullptr || descriptor->provider_id_utf8[0] == '\0' ||
        descriptor->snapshot == nullptr || descriptor->action_handler == nullptr ||
        descriptor->snapshot->kind != cs_managed_callback_kind::entity_snapshot) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const bool use_action_v2 =
        descriptor->action_handler->kind == cs_managed_callback_kind::entity_action_v2;
    if (!use_action_v2 &&
        descriptor->action_handler->kind != cs_managed_callback_kind::entity_action) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    session_call_lease lease;
    int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    auto* session = lease.get();
    if (session->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;

    try {
        entity_registration registration;
        registration.provider_id = descriptor->provider_id_utf8;
        const char* plugin_id = loader::sao_plugins_ctx_plugin_id(session->loader_context);
        if (plugin_id == nullptr || plugin_id[0] == '\0')
            return SAO_ERR_HANDLE_INVALID;
        registration.qualified_provider_id =
            std::string(plugin_id) + "/" + registration.provider_id;
        registration.callbacks = std::make_unique<entity_callback_pair>();
        {
            std::lock_guard lock(session->mutex);
            const auto duplicate =
                std::find_if(session->entities.begin(), session->entities.end(),
                             [&registration](const entity_registration& current) {
                                 return current.provider_id == registration.provider_id;
                             });
            if (duplicate != session->entities.end())
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            if (std::find(session->pending_entity_ids.begin(), session->pending_entity_ids.end(),
                          registration.provider_id) != session->pending_entity_ids.end()) {
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            session->entities.reserve(session->entities.size() +
                                      session->pending_entity_ids.size() + 1);
            session->pending_entity_ids.push_back(registration.provider_id);
        }

        const auto clear_pending = [&session, &registration]() noexcept {
            try {
                std::lock_guard lock(session->mutex);
                std::erase(session->pending_entity_ids, registration.provider_id);
            } catch (...) {
            }
        };

        void* ignored_entry = nullptr;
        status = wrap_managed_callback(session, descriptor->snapshot, &ignored_entry,
                                       &registration.callbacks->snapshot);
        if (status != SAO_OK) {
            clear_pending();
            return status;
        }
        void** action_slot =
            use_action_v2 ? &registration.callbacks->action_v2 : &registration.callbacks->action;
        status = wrap_managed_callback(session, descriptor->action_handler, &ignored_entry,
                                       action_slot);
        if (status != SAO_OK) {
            release_callback_token(registration.callbacks->snapshot);
            clear_pending();
            return status;
        }

        loader::entity_root_contribution_descriptor root{};
        const loader::entity_root_contribution_descriptor* root_ptr = nullptr;
        if (descriptor->contribution_id_utf8 != nullptr &&
            descriptor->contribution_id_utf8[0] != '\0') {
            if (descriptor->root_id_utf8 == nullptr || descriptor->name_utf8 == nullptr) {
                if (use_action_v2)
                    release_callback_token(registration.callbacks->action_v2);
                else
                    release_callback_token(registration.callbacks->action);
                release_callback_token(registration.callbacks->snapshot);
                clear_pending();
                return SAO_ERR_INVALID_ARGUMENT;
            }
            root = {sizeof(root),
                    descriptor->contribution_id_utf8,
                    descriptor->root_id_utf8,
                    descriptor->name_utf8,
                    descriptor->icon_utf8,
                    descriptor->priority};
            root_ptr = &root;
        }
        if (use_action_v2) {
            loader::context_entity_provider_descriptor_v3 native{};
            native.struct_size = sizeof(native);
            native.provider_id_utf8 = registration.provider_id.c_str();
            native.snapshot = callback_entity_snapshot_v2;
            native.action_handler = nullptr;
            native.user_data = registration.callbacks.get();
            native.root_contribution = root_ptr;
            native.action_handler_v2 = callback_entity_action_v2;
            native.action_user_data = registration.callbacks.get();
            native.flags = 0;
            native.reserved = 0;
            status = loader::sao_plugins_ctx_register_entity_provider_v3(session->loader_context,
                                                                         &native);
        } else {
            loader::context_entity_provider_descriptor_v2 native{};
            native.struct_size = sizeof(native);
            native.provider_id_utf8 = registration.provider_id.c_str();
            native.snapshot = callback_entity_snapshot_v2;
            native.action_handler = callback_entity_action;
            native.user_data = registration.callbacks.get();
            native.root_contribution = root_ptr;
            status = loader::sao_plugins_ctx_register_entity_provider_v2(session->loader_context,
                                                                         &native);
        }
        if (status != SAO_OK) {
            if (use_action_v2)
                release_callback_token(registration.callbacks->action_v2);
            else
                release_callback_token(registration.callbacks->action);
            release_callback_token(registration.callbacks->snapshot);
            clear_pending();
            return status;
        }
        {
            std::lock_guard lock(session->mutex);
            session->entities.push_back(std::move(registration));
            std::erase(session->pending_entity_ids, descriptor->provider_id_utf8);
        }
        return SAO_OK;
    } catch (...) {
        try {
            std::lock_guard lock(session->mutex);
            std::erase(session->pending_entity_ids, descriptor->provider_id_utf8);
        } catch (...) {
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL table_emit_context(cs_managed_sdk_session_t opaque, const char* topic_utf8,
                                            const char* payload_json_utf8) {
    if (topic_utf8 == nullptr || topic_utf8[0] == '\0' || payload_json_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    const int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    if (lease.get()->loader_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    return loader::sao_plugins_ctx_emit(lease.get()->loader_context, topic_utf8, payload_json_utf8);
}

int32_t SAO_PLUGINS_CALL table_submit_action_result_v2(cs_managed_sdk_session_t opaque,
                                                       cs_managed_callback_token_t callback_token,
                                                       const cs_managed_action_result_v2* result) {
    if (callback_token == nullptr || result == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    const int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    if (g_pending_action_result.token != callback_token ||
        g_pending_action_result.sink == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (g_pending_action_result.consumed)
        return SAO_ERR_HANDLE_INVALID;
    return forward_managed_action_result(result, &g_pending_action_result);
}

int32_t SAO_PLUGINS_CALL table_last_error(cs_managed_sdk_session_t opaque, char* out_error_utf8,
                                          size_t out_capacity, size_t* out_required) {
    if (out_required == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    const int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    std::string error;
    {
        std::lock_guard lock(lease.get()->mutex);
        error = lease.get()->last_error;
    }
    *out_required = error.size() + 1;
    if (out_error_utf8 == nullptr || out_capacity < *out_required) {
        if (out_error_utf8 != nullptr && out_capacity > 0)
            out_error_utf8[0] = '\0';
        return SAO_ERR_BUFFER_TOO_SMALL;
    }
    std::copy(error.begin(), error.end(), out_error_utf8);
    out_error_utf8[error.size()] = '\0';
    return SAO_OK;
}

// ── ABI1 V3 appended slots: ctx.ui.* spec builder + blocking prompt ───────

int32_t SAO_PLUGINS_CALL table_ui_build(cs_managed_sdk_session_t opaque,
                                        const char* method_utf8,
                                        const char* args_json_utf8, size_t args_size,
                                        char* out_node_json_utf8, size_t out_capacity,
                                        size_t* out_required) {
    if (method_utf8 == nullptr || method_utf8[0] == '\0')
        return SAO_ERR_INVALID_ARGUMENT;
    session_call_lease lease;
    int32_t status = lease.acquire_handle(opaque);
    if (status != SAO_OK)
        return status;
    auto* session = lease.get();
    try {
        nlohmann::json args = nlohmann::json::object();
        if (args_size != 0) {
            if (args_json_utf8 == nullptr || args_size > kMaxDispatchArgsBytes) {
                remember_error(session, SAO_ERR_INVALID_ARGUMENT, "ui_build");
                return SAO_ERR_INVALID_ARGUMENT;
            }
            try {
                args = nlohmann::json::parse(args_json_utf8, args_json_utf8 + args_size);
            } catch (...) {
                remember_error(session, SAO_ERR_INVALID_ARGUMENT, "ui_build");
                return SAO_ERR_INVALID_ARGUMENT;
            }
        }
        nlohmann::json node;
        std::string error;
        if (!script_ctx::script_ui_build(method_utf8, args, node, error)) {
            std::lock_guard lock(session->mutex);
            session->last_error =
                "ui_build(" + std::string(method_utf8) + ") failed: " + error;
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const std::string serialized = node.dump();
        return write_raw_result(serialized.data(), serialized.size(), out_node_json_utf8,
                                out_capacity, out_required);
    } catch (...) {
        remember_error(session, SAO_ERR_OS_CALL_FAILED, "ui_build");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// Null text means user cancellation, not provider failure.
struct prompt_wait_state {
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    int32_t pressed_button = -1;
    bool has_text = false;
    int32_t status = SAO_OK;
    std::string text;
};

void SAO_SDK_CALL prompt_dialog_callback(sao_sdk_dialog_token_t, int32_t pressed_button,
                                         const char* input_text_utf8, size_t input_text_len,
                                         void* user_data) {
    auto* state = static_cast<prompt_wait_state*>(user_data);
    if (state == nullptr)
        return;
    try {
        std::lock_guard lock(state->mutex);
        state->pressed_button = pressed_button;
        try {
            if (input_text_utf8 != nullptr) {
                if (input_text_len > 1024u * 1024u)
                    state->status = SAO_ERR_INVALID_ARGUMENT;
                else
                    state->text.assign(input_text_utf8, input_text_len);
            }
        } catch (...) {
            state->status = SAO_ERR_OS_CALL_FAILED;
        }
        state->has_text = input_text_utf8 != nullptr;
        state->done = true;
        state->ready.notify_all();
    } catch (...) {
    }
}

int32_t SAO_PLUGINS_CALL table_prompt(cs_managed_sdk_session_t opaque,
                                      const char* title_utf8, const char* current_utf8,
                                      char* out_result_json_utf8, size_t out_capacity,
                                      size_t* out_required) {
    if (out_required == nullptr || (out_result_json_utf8 == nullptr && out_capacity != 0))
        return SAO_ERR_INVALID_ARGUMENT;
    *out_required = 0;
    if (g_prompt_wait_active)
        return loader::SAO_PLUGINS_ERR_BUSY;
    session_call_lease lease;
    const int32_t lease_status = lease.acquire_handle(opaque);
    if (lease_status != SAO_OK)
        return lease_status;
    auto* session = lease.get();
    if (session->sdk_context == nullptr)
        return SAO_ERR_NOT_INITIALIZED;

    sao_sdk_dialog_token_t dialog = 0;
    try {
        auto state = std::make_shared<prompt_wait_state>();
        {
            // Dismiss may fail while a callback is active; SDK teardown precedes session destruction.
            std::lock_guard lock(session->mutex);
            if (session->prompt_active || session->retiring)
                return loader::SAO_PLUGINS_ERR_BUSY;
            session->prompt_states.push_back(state);
            session->prompt_active = true;
        }
        struct PromptGuard {
            sdk_bridge_session* session;
            ~PromptGuard() {
                std::lock_guard lock(session->mutex);
                session->prompt_active = false;
                g_prompt_wait_active = false;
            }
        } prompt_guard{session};
        g_prompt_wait_active = true;
        const auto retiring = [session] {
            std::lock_guard lock(session->mutex);
            return session->retiring;
        };
        SaoSdkDialogSpec spec{};
        spec.kind = SAO_SDK_DIALOG_INPUT;
        spec.title_utf8 = title_utf8 != nullptr ? title_utf8 : "";
        spec.message_utf8 = "";
        spec.input_prompt_utf8 = title_utf8 != nullptr ? title_utf8 : "";
        spec.input_default_utf8 = current_utf8 != nullptr ? current_utf8 : "";
        spec.input_max_length = 65536;
        spec.dismiss_on_focus_out = false;
        spec.dismiss_on_esc = true;
        const sao_sdk_status_t shown = sao_sdk_dialog_show(
            session->sdk_context, &spec, prompt_dialog_callback, state.get(), &dialog);
        if (shown != SAO_SDK_OK) {
            remember_error(session, shown, "prompt dialog_show");
            switch (shown) {
            case SAO_SDK_ERR_BUSY: return loader::SAO_PLUGINS_ERR_BUSY;
            case SAO_SDK_ERR_UNSUPPORTED: return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
            case SAO_SDK_ERR_ABI_MISMATCH: return loader::SAO_PLUGINS_ERR_ABI_MISMATCH;
            case SAO_SDK_ERR_ACCESS_DENIED: return loader::SAO_PLUGINS_ERR_NOT_OWNER;
            case SAO_SDK_ERR_ALREADY_EXISTS: return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            case SAO_SDK_ERR_NOT_FOUND: return SAO_ERR_HANDLE_INVALID;
            case SAO_SDK_ERR_INTERNAL: return SAO_ERR_OS_CALL_FAILED;
            default: return shown;
            }
        }
        ordered_json result;
        int32_t status = SAO_OK;
#if defined(_WIN32) && defined(SAO_CSHARP_HAS_UI)
        void* compositor_ptr = nullptr;
        (void)sao_sdk_platform_get_ui_compositor(&compositor_ptr);
        const auto compositor = static_cast<sao_ui_compositor_handle_t>(compositor_ptr);
        if (compositor != nullptr && sao_ui_compositor_require_owner_thread(compositor) == SAO_STATUS_OK) {
            const auto deadline = std::chrono::steady_clock::now() + kPromptWaitBudget;
            while (status == SAO_OK) {
                if (retiring()) {
                    status = loader::SAO_PLUGINS_ERR_BUSY;
                    break;
                }
                {
                    std::lock_guard lock(state->mutex);
                    if (state->done)
                        break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    status = SAO_STATUS_ERR_TIMEOUT;
                    break;
                }
                MSG message{};
                for (size_t count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                    if (message.message == WM_QUIT) {
                        PostQuitMessage(static_cast<int>(message.wParam));
                        status = SAO_STATUS_ERR_CANCELLED;
                        break;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (status == SAO_OK && retiring())
                    status = loader::SAO_PLUGINS_ERR_BUSY;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->done)
                        break;
                }
                if (status == SAO_OK)
                    status = sao_ui_compositor_tick(compositor);
                if (status == SAO_OK)
                    (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            }
        } else
#endif
        {
            const auto deadline = std::chrono::steady_clock::now() + kPromptWaitBudget;
            for (;;) {
                if (retiring()) {
                    status = loader::SAO_PLUGINS_ERR_BUSY;
                    break;
                }
                std::unique_lock lock(state->mutex);
                if (state->ready.wait_for(lock, std::chrono::milliseconds(16), [&state] { return state->done; }))
                    break;
                if (std::chrono::steady_clock::now() >= deadline) {
                    status = SAO_STATUS_ERR_TIMEOUT;
                    break;
                }
            }
        }
        {
            std::lock_guard lock(state->mutex);
            if (status == SAO_OK && state->status != SAO_OK)
                status = state->status;
            else if (status == SAO_OK)
                result["text"] = state->pressed_button == SAO_SDK_DIALOG_BUTTON_OK && state->has_text
                    ? ordered_json(state->text) : ordered_json(nullptr);
        }
        const int32_t dismissed = sao_sdk_dialog_dismiss(session->sdk_context, dialog);
        if (dismissed == SAO_SDK_OK || dismissed == SAO_SDK_ERR_NOT_FOUND ||
            dismissed == SAO_SDK_ERR_HANDLE_INVALID) {
            std::lock_guard lock(session->mutex);
            std::erase(session->prompt_states, state);
            dialog = 0;
        } else {
            remember_error(session, dismissed, "prompt dialog_dismiss");
        }
        if (status != SAO_OK) {
            remember_error(session, status, "prompt wait");
            return status;
        }
        const std::string serialized = result.dump();
        return write_raw_result(serialized.data(), serialized.size(), out_result_json_utf8,
                                out_capacity, out_required);
    } catch (...) {
        if (dialog != 0)
            (void)sao_sdk_dialog_dismiss(session->sdk_context, dialog);
        remember_error(session, SAO_ERR_OS_CALL_FAILED, "prompt");
        return SAO_ERR_OS_CALL_FAILED;
    }
}

// Canonical csharp ctx surface — every name bound by the managed ctx facade.
// Names whose native surface is optional (compositor gpu/mmf, run_on_ui, …)
// are still marked bound: ctx_surface records binding presence, while the
// dispatch resolves availability per provider and fails closed.
const char* const kCsharpCtxSurfaceNames[] = {
    "plugin_id",      "path",            "web_path",        "assets_path",
    "base_dir",       "should_stop",     "event_bus",       "owner",
    "engine",         "ui",              "mem",
    "log",            "log_info",        "log_warn",        "log_error",
    "subscribe",      "subscribe_once",  "unsubscribe",     "emit",
    "on_damage",      "on_heal",         "on_skill",        "on_boss",
    "on_snapshot",    "on_encounter_finalized",
    "get_snapshot",   "snapshot_value",  "recent_events",   "time",
    "get_setting",    "setting",         "set_setting",     "set_defaults",
    "register_parser_adapter",           "register_exporter",
    "register_formatter",                "register_trigger_type",
    "register_report_view",              "register_timer",
    "register_ui_panel",                 "register_render_hook",
    "set_overlay",    "clear_overlay",   "request_redraw",
    "register_hotkey","register_engine", "get_engine",      "require_engine",
    "call_engine",    "call_runtime",
    "register_data_source",              "register_menu_category",
    "register_menu_surface",             "register_action_handler",
    "set_interval",   "set_timeout",     "clear_timer",     "run_on_ui",
    "notify",         "dismiss_notify",  "toast",
    "open_file",      "open_window",
    "create_compositor_layer",           "upload_compositor_frame",
    "set_compositor_layer_mmf_source",   "set_compositor_layer_shared_texture_source",
    "set_compositor_layer_position",     "set_compositor_layer_visible",
    "set_compositor_layer_input",        "destroy_compositor_layer",
    "compositor_gpu_interop_available",  "compositor_layer_shared_texture_active",
    "compositor_display_refresh_hz",
    "ensure_requirements",               "load_local",      "prompt",
    "register_thread",
    "ui.panel",       "ui.section",      "ui.card",         "ui.row",
    "ui.group",       "ui.text",         "ui.title",        "ui.kv",
    "ui.bar",         "ui.slider",       "ui.badge",        "ui.divider",
    "ui.spacer",      "ui.button",       "ui.input",        "ui.table",
    "ui.canvas",      "ui.rgba_frame",   "ui.rect",         "ui.oval",
    "ui.line",        "ui.ctext",
    nullptr,
};

void note_csharp_ctx_surface() noexcept {
    static std::once_flag noted;
    std::call_once(noted, [] {
        script_ctx::ctx_surface_note_all(loader::engine_kind::csharp,
                                         kCsharpCtxSurfaceNames);
    });
}

const cs_managed_sdk_table kSdkTable{
    sizeof(cs_managed_sdk_table),
    SAO_CSHOST_SDK_TABLE_ABI_VERSION,
    table_dispatch,
    table_release_callback,
    table_log,
    table_register_engine,
    table_get_engine,
    table_register_entity_provider,
    table_unregister_entity_provider,
    table_last_error,
    table_register_entity_provider_v2,
    table_emit_context,
    table_submit_action_result_v2,
    table_ui_build,
    table_prompt,
};

int32_t quiesce_entities(sdk_bridge_session* session) noexcept {
    try {
        std::vector<std::string> owned_ids;
        {
            std::lock_guard lock(session->mutex);
            owned_ids.reserve(session->entities.size());
            for (const auto& registration : session->entities)
                owned_ids.push_back(registration.qualified_provider_id);
        }
        if (owned_ids.empty() || session->loader_context == nullptr)
            return SAO_OK;
        std::vector<const char*> ids;
        ids.reserve(owned_ids.size());
        for (const auto& id : owned_ids)
            ids.push_back(id.c_str());
        return loader::plugin_context_unregister_entity_providers(session->loader_context,
                                                                  ids.data(), ids.size());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace

const cs_managed_sdk_table* cshost_sdk_bridge_table() noexcept {
    return &kSdkTable;
}

int32_t cshost_sdk_session_create(void* runtime, void* loader_context, void* sdk_context,
                                  managed_component_s* component, bool retain_context,
                                  sdk_bridge_session** out_session) noexcept {
    if (runtime == nullptr || out_session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_session = nullptr;
    std::unique_ptr<sdk_bridge_session> session;
    bool registered = false;
    bool published = false;
    try {
        session = std::make_unique<sdk_bridge_session>();
        session->runtime = runtime;
        session->loader_context = static_cast<plugin_context_t*>(loader_context);
        session->sdk_context = static_cast<SaoSdkContext*>(sdk_context);
        session->component = component;
        if (retain_context && session->loader_context != nullptr) {
            const int32_t status =
                loader::plugin_context_retain_host_lease(session->loader_context);
            if (status != SAO_OK)
                return status;
            session->context_lease = true;
        }
        {
            std::lock_guard lock(g_sessions_mutex);
            if (g_sessions.find(runtime) != g_sessions.end()) {
                if (session->context_lease)
                    loader::plugin_context_release_host_lease(session->loader_context);
                return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            do {
                session->handle = next_session_handle();
            } while (g_session_handles.find(session->handle) != g_session_handles.end());
            g_sessions.emplace(runtime, session.get());
            try {
                g_session_handles.emplace(session->handle, session.get());
            } catch (...) {
                g_sessions.erase(runtime);
                throw;
            }
            registered = true;
        }
        if (component != nullptr) {
            const int32_t status = cshost_component_publish_sdk_session(
                component, &kSdkTable, reinterpret_cast<void*>(session->handle));
            if (status != SAO_OK) {
                std::lock_guard lock(g_sessions_mutex);
                g_sessions.erase(runtime);
                g_session_handles.erase(session->handle);
                if (session->context_lease)
                    loader::plugin_context_release_host_lease(session->loader_context);
                return status;
            }
            published = true;
        }
        // Record the canonical csharp ctx surface once per process; the note
        // table is a global set keyed by engine kind.
        note_csharp_ctx_surface();
        *out_session = session.release();
        return SAO_OK;
    } catch (...) {
        if (published && component != nullptr && session != nullptr)
            cshost_component_clear_sdk_session(component, reinterpret_cast<void*>(session->handle));
        if (registered && session != nullptr) {
            try {
                std::lock_guard lock(g_sessions_mutex);
                g_sessions.erase(runtime);
                g_session_handles.erase(session->handle);
            } catch (...) {
            }
        }
        if (session != nullptr && session->context_lease)
            loader::plugin_context_release_host_lease(session->loader_context);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sdk_bridge_session* cshost_sdk_session_find(void* runtime) noexcept {
    try {
        std::lock_guard lock(g_sessions_mutex);
        const auto found = g_sessions.find(runtime);
        return found == g_sessions.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

sdk_bridge_session* cshost_sdk_session_find_handle(cs_managed_sdk_session_t opaque) noexcept {
    const uintptr_t handle = reinterpret_cast<uintptr_t>(opaque);
    if (handle == 0)
        return nullptr;
    try {
        std::lock_guard lock(g_sessions_mutex);
        const auto found = g_session_handles.find(handle);
        return found == g_session_handles.end() ? nullptr : found->second;
    } catch (...) {
        return nullptr;
    }
}

cs_managed_sdk_session_t cshost_sdk_session_handle(sdk_bridge_session* session) noexcept {
    return session == nullptr ? nullptr : reinterpret_cast<void*>(session->handle);
}

int32_t cshost_sdk_session_quiesce(sdk_bridge_session* session) noexcept {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(session->mutex);
        if (session_active_on_current_thread(session))
            return loader::SAO_PLUGINS_ERR_BUSY;
        if (session->retiring)
            return session->active_calls == 0 ? SAO_OK : loader::SAO_PLUGINS_ERR_BUSY;
        session->retiring = true;
        session->idle.wait(lock, [session] { return session->active_calls == 0; });
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_sdk_session_resume(sdk_bridge_session* session) noexcept {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(session->mutex);
        if (!session->retiring || session->active_calls != 0)
            return loader::SAO_PLUGINS_ERR_BUSY;
        session->retiring = false;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_sdk_session_release_callbacks(sdk_bridge_session* session) noexcept {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        for (;;) {
            loader_panel_ud* panel = nullptr;
            {
                std::lock_guard lock(session->mutex);
                if (!session->retiring || session->active_calls != 0)
                    return loader::SAO_PLUGINS_ERR_BUSY;
                if (session->panel_ud_pool.empty())
                    break;
                panel = session->panel_ud_pool.back().get();
            }
            const int32_t status = loader::sao_plugins_ctx_unregister_ui_panel(
                session->loader_context, panel->surface.c_str());
            if (status != SAO_OK && status != SAO_ERR_HANDLE_INVALID)
                return status;
            std::lock_guard lock(session->mutex);
            session->panel_ud_pool.pop_back();
        }
        const int32_t entity_status = quiesce_entities(session);
        if (entity_status != SAO_OK)
            return entity_status;
        while (true) {
            uintptr_t callback_token = 0;
            {
                std::lock_guard lock(session->mutex);
                if (!session->retiring || session->active_calls != 0)
                    return loader::SAO_PLUGINS_ERR_BUSY;
                if (session->callbacks.empty()) {
                    const int32_t status = session->callback_release_status;
                    session->callback_release_status = SAO_OK;
                    return status;
                }
                callback_token = session->callbacks.back()->token;
            }
            release_callback_token(reinterpret_cast<void*>(callback_token));
            {
                std::lock_guard lock(session->mutex);
                if (std::any_of(session->callbacks.begin(), session->callbacks.end(),
                                [callback_token](const managed_callback* callback) {
                                    return callback->token == callback_token;
                                })) {
                    return loader::SAO_PLUGINS_ERR_BUSY;
                }
            }
        }
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_sdk_session_clear_sdk_context(sdk_bridge_session* session,
                                             void* sdk_context) noexcept {
    if (session == nullptr || sdk_context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(session->mutex);
        if (!session->retiring || session->active_calls != 0 ||
            session->sdk_context != sdk_context) {
            return loader::SAO_PLUGINS_ERR_BUSY;
        }
        session->sdk_context = nullptr;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_sdk_session_set_binding(sdk_bridge_session* session, void* binding) noexcept {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::lock_guard lock(session->mutex);
        if (binding != nullptr && session->binding != nullptr)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        if (binding == nullptr && !session->retiring)
            return loader::SAO_PLUGINS_ERR_BUSY;
        session->binding = binding;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_sdk_session_finish(sdk_bridge_session* session) noexcept {
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        {
            std::lock_guard lock(session->mutex);
            if (!session->retiring || session->binding != nullptr || session->active_calls != 0)
                return loader::SAO_PLUGINS_ERR_BUSY;
            if (session->sdk_context != nullptr && !session->prompt_states.empty())
                return loader::SAO_PLUGINS_ERR_BUSY;
        }
        const int32_t release_status = cshost_sdk_session_release_callbacks(session);
        if (release_status != SAO_OK)
            return release_status;
        int32_t status = SAO_OK;
        {
            std::lock_guard lock(session->mutex);
            status = session->callback_release_status;
            session->callback_release_status = SAO_OK;
            if (status != SAO_OK)
                return status;
            session->entities.clear();
        }
        if (session->component != nullptr)
            cshost_component_clear_sdk_session(session->component,
                                               reinterpret_cast<void*>(session->handle));
        {
            std::lock_guard lock(g_sessions_mutex);
            const auto runtime = g_sessions.find(session->runtime);
            if (runtime != g_sessions.end() && runtime->second == session)
                g_sessions.erase(runtime);
            const auto found = g_session_handles.find(session->handle);
            if (found != g_session_handles.end() && found->second == session)
                g_session_handles.erase(found);
        }
        if (session->context_lease) {
            loader::plugin_context_release_host_lease(session->loader_context);
            session->context_lease = false;
        }
        delete session;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t cshost_sdk_session_discard(sdk_bridge_session* session) noexcept {
    return cshost_sdk_session_finish(session);
}

int32_t cshost_sdk_wrap_callback(sdk_bridge_session* session,
                                 const cs_managed_callback_descriptor* descriptor,
                                 void** out_callback, void** out_user_data) noexcept {
    if (session == nullptr || !valid_callback_descriptor(descriptor) || out_callback == nullptr ||
        out_user_data == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    session_call_lease lease;
    int32_t status = lease.acquire(session);
    if (status != SAO_OK)
        return status;
    auto callback = std::unique_ptr<managed_callback>(new (std::nothrow) managed_callback{});
    if (callback == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    callback->owner = session;
    callback->descriptor = *descriptor;
    managed_lifetime_call lifetime{callback.get(), true};
    status = sdk_binding::sao_plugins_binding_barrier(invoke_managed_lifetime, &lifetime, nullptr);
    if (status != SAO_OK)
        return status;
    try {
        std::lock_guard callback_lock(g_callbacks_mutex);
        do {
            callback->token = next_callback_token();
        } while (g_callbacks.find(callback->token) != g_callbacks.end());
        std::lock_guard session_lock(session->mutex);
        session->callbacks.push_back(callback.get());
        const auto [_, inserted] = g_callbacks.emplace(callback->token, callback.get());
        if (!inserted)
            throw std::bad_alloc();
    } catch (...) {
        try {
            std::lock_guard callback_lock(g_callbacks_mutex);
            g_callbacks.erase(callback->token);
            std::lock_guard session_lock(session->mutex);
            const auto found =
                std::find(session->callbacks.begin(), session->callbacks.end(), callback.get());
            if (found != session->callbacks.end())
                session->callbacks.erase(found);
        } catch (...) {
        }
        lifetime.retain = false;
        (void)sdk_binding::sao_plugins_binding_barrier(invoke_managed_lifetime, &lifetime, nullptr);
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_callback = callback_entry(descriptor->kind);
    *out_user_data = reinterpret_cast<void*>(callback->token);
    callback.release();
    return SAO_OK;
}

int32_t cshost_sdk_wrap_callback_for_runtime(void* runtime,
                                             const cs_managed_callback_descriptor* descriptor,
                                             void** out_callback, void** out_user_data) noexcept {
    session_call_lease lease;
    const int32_t status = acquire_runtime_session(runtime, lease);
    if (status != SAO_OK)
        return status;
    return cshost_sdk_wrap_callback(lease.get(), descriptor, out_callback, out_user_data);
}

void cshost_sdk_release_provider_callback(void* callback_user_data) noexcept {
    release_managed_callback(callback_user_data);
}

int32_t cshost_sdk_bridge_set_binding(sdk_bridge_session* session,
                                      sdk_binding::plugin_binding_handle_t binding) noexcept {
    return cshost_sdk_session_set_binding(session, binding);
}

int32_t cshost_sdk_bridge_finish(sdk_bridge_session* session) noexcept {
    return cshost_sdk_session_finish(session);
}

int32_t cshost_sdk_bridge_discard(sdk_bridge_session* session) noexcept {
    return cshost_sdk_session_discard(session);
}

} // namespace sao::plugins::csharp_host
