#include "cs_component_internal.h"
#include "cs_sdk_bridge_internal.h"

#include "sao/plugins/loader/entity_provider.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/sdk/sao_sdk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sao::plugins::csharp_host {
namespace {

using loader::plugin_context_t;
using sdk_binding::language_binding_operation;
using sdk_binding::language_binding_request;
using sdk_binding::language_host_kind;
using sdk_binding::sdk_context_call_request;
using sdk_binding::sdk_method_id;

struct managed_callback;

struct entity_callback_pair {
    void* snapshot = nullptr;
    void* action = nullptr;
};

struct entity_registration {
    std::string provider_id;
    std::unique_ptr<entity_callback_pair> callbacks;
};

} // namespace

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

int32_t SAO_PLUGINS_CALL callback_entity_action(const char* action_id_utf8,
                                                const char* payload_json_utf8, void* user_data) {
    auto* pair = static_cast<entity_callback_pair*>(user_data);
    const cs_managed_entity_action_invocation invocation{action_id_utf8, payload_json_utf8};
    return pair == nullptr ? SAO_ERR_INVALID_ARGUMENT : invoke_callback(pair->action, &invocation);
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
        return reinterpret_cast<void*>(&invoke_managed);
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
            session->entities.reserve(session->entities.size() + 1);
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
    {
        std::lock_guard lock(session->mutex);
        const auto found = std::find_if(session->entities.begin(), session->entities.end(),
                                        [provider_id_utf8](const entity_registration& current) {
                                            return current.provider_id == provider_id_utf8;
                                        });
        if (found == session->entities.end())
            return SAO_ERR_HANDLE_INVALID;
        callbacks = found->callbacks.get();
    }
    const char* ids[] = {provider_id_utf8};
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
        release_callback_token(owned_callbacks->action);
        release_callback_token(owned_callbacks->snapshot);
    }
    return SAO_OK;
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
};

int32_t quiesce_entities(sdk_bridge_session* session) noexcept {
    try {
        std::vector<std::string> owned_ids;
        {
            std::lock_guard lock(session->mutex);
            owned_ids.reserve(session->entities.size());
            for (const auto& registration : session->entities)
                owned_ids.push_back(registration.provider_id);
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
        const int32_t entity_status = quiesce_entities(session);
        if (entity_status != SAO_OK)
            return entity_status;
        while (true) {
            uintptr_t callback_token = 0;
            {
                std::lock_guard lock(session->mutex);
                if (!session->retiring || session->active_calls != 0)
                    return loader::SAO_PLUGINS_ERR_BUSY;
                if (session->callbacks.empty())
                    return session->callback_release_status;
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
        }
        while (true) {
            uintptr_t callback_token = 0;
            {
                std::lock_guard lock(session->mutex);
                if (session->callbacks.empty())
                    break;
                callback_token = session->callbacks.back()->token;
            }
            release_managed_callback(reinterpret_cast<void*>(callback_token));
        }
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
