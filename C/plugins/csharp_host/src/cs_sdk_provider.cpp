#include "cs_component_internal.h"
#include "cs_sdk_bridge_internal.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/sdk_binding/binding_common.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sao::plugins::csharp_host {
namespace {

using sdk_binding::language_binding_operation;
using sdk_binding::language_binding_request;
using sdk_binding::language_host_adapter_vtable;
using sdk_binding::language_host_kind;
using sdk_binding::plugin_binding_handle_t;
using sdk_binding::sdk_method_id;

struct pending_session {
    void* binding_context = nullptr;
    void* loader_context = nullptr;
    void* sdk_context = nullptr;
    managed_component_s* component = nullptr;
    bool retain_context = false;
};

std::mutex g_pending_mutex;
std::unordered_map<void*, pending_session> g_pending;
std::mutex g_test_bindings_mutex;
std::unordered_map<sdk_bridge_session*, plugin_binding_handle_t> g_test_bindings;

bool SAO_PLUGINS_CALL provider_available(void*) {
    return true;
}

int32_t SAO_PLUGINS_CALL provider_load(void* context, void* runtime, void** out_plugin, void*) {
    if (context == nullptr || runtime == nullptr || out_plugin == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    try {
        pending_session pending;
        {
            std::lock_guard lock(g_pending_mutex);
            const auto found = g_pending.find(runtime);
            if (found == g_pending.end() || found->second.binding_context != context)
                return SAO_ERR_NOT_INITIALIZED;
            pending = found->second;
            g_pending.erase(found);
        }
        sdk_bridge_session* session = nullptr;
        const int32_t status =
            cshost_sdk_session_create(runtime, pending.loader_context, pending.sdk_context,
                                      pending.component, pending.retain_context, &session);
        if (status == SAO_OK)
            *out_plugin = session;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL provider_unload(void* plugin, void*) {
    return cshost_sdk_session_quiesce(static_cast<sdk_bridge_session*>(plugin));
}

int32_t SAO_PLUGINS_CALL provider_invoke(void* plugin, const char* method_name_utf8,
                                         const uint8_t* args_json_utf8, size_t args_size,
                                         uint8_t* out_result_json_utf8, size_t out_capacity,
                                         size_t* out_required, char*, size_t, void*) {
    if (method_name_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const auto method = sdk_binding::sao_plugins_binding_method_from_name(method_name_utf8);
    if (method == sdk_method_id::method_count_)
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    cs_managed_sdk_call call{};
    call.struct_size = sizeof(call);
    call.method_id = static_cast<uint16_t>(method);
    call.args_json_utf8 = reinterpret_cast<const char*>(args_json_utf8);
    call.args_size = args_size;
    call.out_result_json_utf8 = reinterpret_cast<char*>(out_result_json_utf8);
    call.out_capacity = out_capacity;
    call.out_required = out_required;
    return cshost_sdk_bridge_table()->dispatch(plugin, &call);
}

int32_t SAO_PLUGINS_CALL provider_dispatch(language_binding_operation operation,
                                           language_binding_request* request, void*) {
    if (request == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (operation != language_binding_operation::callback_wrap)
        return loader::SAO_PLUGINS_ERR_UNSUPPORTED;
    return cshost_sdk_wrap_callback_for_runtime(
        request->runtime, static_cast<cs_managed_callback_descriptor*>(request->value),
        request->out_callback, request->out_user_data);
}

void SAO_PLUGINS_CALL provider_release_callback(void* callback_user_data, void*) {
    cshost_sdk_release_provider_callback(callback_user_data);
}

language_host_adapter_vtable provider_vtable() {
    language_host_adapter_vtable table{};
    table.language = language_host_kind::csharp;
    table.available = provider_available;
    table.load_plugin = provider_load;
    table.unload_plugin = provider_unload;
    table.invoke = provider_invoke;
    table.dispatch = provider_dispatch;
    table.release_callback = provider_release_callback;
    return table;
}

} // namespace

int32_t cshost_sdk_bridge_prepare(void* runtime, void* loader_context, void* sdk_context,
                                  managed_component_s* component) noexcept {
    if (runtime == nullptr || loader_context == nullptr || sdk_context == nullptr ||
        component == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(g_pending_mutex);
        if (g_pending.find(runtime) != g_pending.end() ||
            cshost_sdk_session_find(runtime) != nullptr)
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        g_pending.emplace(
            runtime, pending_session{loader_context, loader_context, sdk_context, component, true});
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

void cshost_sdk_bridge_cancel(void* runtime) noexcept {
    try {
        std::lock_guard lock(g_pending_mutex);
        g_pending.erase(runtime);
    } catch (...) {
    }
}

int32_t cshost_register_sdk_binding_provider() noexcept {
    const auto table = provider_vtable();
    return sdk_binding::sao_plugins_binding_register_language_host(&table);
}

int32_t cshost_unregister_sdk_binding_provider() noexcept {
    return sdk_binding::sao_plugins_binding_unregister_language_host(language_host_kind::csharp);
}

int32_t cshost_sdk_bridge_test_start(void* runtime, void* loader_context, void* sdk_context,
                                     const cs_managed_sdk_table** out_table,
                                     cs_managed_sdk_session_t* out_session) noexcept {
    if (runtime == nullptr || out_table == nullptr || out_session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_table = nullptr;
    *out_session = nullptr;
    int32_t status = cshost_register_sdk_binding_provider();
    if (status != SAO_OK)
        return status;
    {
        std::lock_guard lock(g_pending_mutex);
        if (g_pending.find(runtime) != g_pending.end()) {
            (void)cshost_unregister_sdk_binding_provider();
            return loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        g_pending.emplace(runtime,
                          pending_session{runtime, loader_context, sdk_context, nullptr, false});
    }
    plugin_binding_handle_t binding = nullptr;
    status = sdk_binding::sao_plugins_binding_csharp_activate(
        reinterpret_cast<sdk_binding::plugin_context_ptr>(runtime),
        reinterpret_cast<sdk_binding::csharp_domain_ptr>(runtime), &binding);
    if (status != SAO_OK) {
        cshost_sdk_bridge_cancel(runtime);
        (void)cshost_unregister_sdk_binding_provider();
        return status;
    }
    auto* session = cshost_sdk_session_find(runtime);
    if (session == nullptr) {
        (void)sdk_binding::sao_plugins_binding_csharp_deactivate(binding);
        (void)cshost_unregister_sdk_binding_provider();
        return SAO_ERR_NOT_INITIALIZED;
    }
    status = cshost_sdk_session_set_binding(session, binding);
    if (status != SAO_OK) {
        (void)sdk_binding::sao_plugins_binding_csharp_deactivate(binding);
        (void)cshost_sdk_session_quiesce(session);
        (void)cshost_sdk_session_discard(session);
        (void)cshost_unregister_sdk_binding_provider();
        return status;
    }
    {
        std::lock_guard lock(g_test_bindings_mutex);
        g_test_bindings.emplace(session, binding);
    }
    *out_table = cshost_sdk_bridge_table();
    *out_session = cshost_sdk_session_handle(session);
    return SAO_OK;
}

int32_t cshost_sdk_bridge_test_finish(cs_managed_sdk_session_t opaque) noexcept {
    auto* session = cshost_sdk_session_find_handle(opaque);
    if (session == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    const int32_t quiesce_status = cshost_sdk_session_quiesce(session);
    if (quiesce_status != SAO_OK)
        return quiesce_status;
    const int32_t release_status = cshost_sdk_session_release_callbacks(session);
    plugin_binding_handle_t binding = nullptr;
    {
        std::lock_guard lock(g_test_bindings_mutex);
        const auto found = g_test_bindings.find(session);
        if (found == g_test_bindings.end())
            return SAO_ERR_HANDLE_INVALID;
        binding = found->second;
    }
    const int32_t deactivate_status = sdk_binding::sao_plugins_binding_csharp_deactivate(binding);
    if (deactivate_status != SAO_OK)
        return deactivate_status;
    {
        std::lock_guard lock(g_test_bindings_mutex);
        g_test_bindings.erase(session);
    }
    (void)cshost_sdk_session_set_binding(session, nullptr);
    const int32_t finish_status = cshost_sdk_session_finish(session);
    if (finish_status != SAO_OK)
        (void)cshost_sdk_session_finish(session);
    const int32_t unregister_status = cshost_unregister_sdk_binding_provider();
    if (release_status != SAO_OK)
        return release_status;
    return finish_status != SAO_OK ? finish_status : unregister_status;
}

} // namespace sao::plugins::csharp_host
