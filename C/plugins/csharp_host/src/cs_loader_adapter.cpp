#include "sao/plugins/csharp_host/cs_loader_adapter.h"

#include "cs_component_internal.h"
#include "cs_sdk_bridge_internal.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/sdk_binding/binding_csharp.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace sao::plugins::csharp_host {

using loader_plugin_handle_t = sao::plugins::loader::plugin_handle_t;
using plugin_context_t = sao::plugins::loader::plugin_context_t;

struct adapter_plugin_record {
    enum class state {
        loading,
        ready,
        unloading,
    } lifecycle = state::loading;
    managed_component_s* component = nullptr;
    plugin_context_t* context = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    sao::plugins::sdk_binding::plugin_binding_handle_t binding = nullptr;
    sdk_bridge_session* sdk_session = nullptr;
    size_t active_calls = 0;
    bool on_load_succeeded = false;
};

struct cs_loader_adapter_owner_s {
    cs_host_handle_t host = nullptr;
    bool active = false;
    std::unordered_map<loader_plugin_handle_t, adapter_plugin_record> plugins;
    std::unordered_map<loader_plugin_handle_t, std::string> last_errors;
};

namespace {

std::mutex g_adapter_mutex;
cs_loader_adapter_owner_s* g_adapter_owner = nullptr;

cs_loader_adapter_owner_s* active_owner(void* host_user_data) {
    auto* owner = static_cast<cs_loader_adapter_owner_s*>(host_user_data);
    return owner != nullptr && owner == g_adapter_owner && owner->active ? owner : nullptr;
}

void retain_error(cs_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                  std::string error) {
    std::lock_guard lock(g_adapter_mutex);
    if (owner == g_adapter_owner && owner->active) {
        owner->last_errors[plugin] = std::move(error);
    }
}

void clear_error(cs_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin) {
    std::lock_guard lock(g_adapter_mutex);
    if (owner == g_adapter_owner && owner->active)
        owner->last_errors.erase(plugin);
}

class plugin_call_lease {
  public:
    plugin_call_lease() = default;
    ~plugin_call_lease() {
        reset();
    }

    plugin_call_lease(const plugin_call_lease&) = delete;
    plugin_call_lease& operator=(const plugin_call_lease&) = delete;

    void assign(cs_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                managed_component_s* component, plugin_context_t* context, bool on_load_succeeded) {
        owner_ = owner;
        plugin_ = plugin;
        component_ = component;
        context_ = context;
        on_load_succeeded_ = on_load_succeeded;
    }

    managed_component_s* component() const {
        return component_;
    }
    plugin_context_t* context() const {
        return context_;
    }
    bool on_load_succeeded() const {
        return on_load_succeeded_;
    }

  private:
    void reset() noexcept {
        if (owner_ == nullptr)
            return;
        try {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner_->plugins.find(plugin_);
            if (found != owner_->plugins.end() && found->second.component == component_ &&
                found->second.active_calls > 0) {
                --found->second.active_calls;
            }
        } catch (...) {
        }
        owner_ = nullptr;
        plugin_ = nullptr;
        component_ = nullptr;
        context_ = nullptr;
        on_load_succeeded_ = false;
    }

    cs_loader_adapter_owner_s* owner_ = nullptr;
    loader_plugin_handle_t plugin_ = nullptr;
    managed_component_s* component_ = nullptr;
    plugin_context_t* context_ = nullptr;
    bool on_load_succeeded_ = false;
};

int32_t acquire_plugin(void* host_user_data, loader_plugin_handle_t plugin,
                       plugin_call_lease& lease) {
    std::lock_guard lock(g_adapter_mutex);
    auto* owner = active_owner(host_user_data);
    if (owner == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    const auto found = owner->plugins.find(plugin);
    if (found == owner->plugins.end() || found->second.component == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second.lifecycle != adapter_plugin_record::state::ready) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second.active_calls;
    lease.assign(owner, plugin, found->second.component, found->second.context,
                 found->second.on_load_succeeded);
    return SAO_OK;
}

int32_t invoke_hook(loader_plugin_handle_t plugin, void* host_user_data, managed_hook hook,
                    const char* hook_name, bool optional, void* argument, int32_t argument_size,
                    int32_t* managed_result) {
    plugin_call_lease lease;
    int32_t status = acquire_plugin(host_user_data, plugin, lease);
    if (status != SAO_OK)
        return status;
    if (!cshost_component_has_hook(lease.component(), hook)) {
        if (optional) {
            if (managed_result != nullptr)
                *managed_result = 0;
            return SAO_OK;
        }
        retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                     std::string("required managed hook is missing: ") + hook_name);
        return SAO_ERR_HANDLE_INVALID;
    }
    std::string error;
    status = cshost_component_invoke(lease.component(), hook, argument, argument_size,
                                     managed_result, error);
    if (status != SAO_OK) {
        retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                     error.empty() ? std::string("managed hook failed: ") + hook_name
                                   : std::string(hook_name) + ": " + error);
    }
    return status;
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin, void* host_user_data);

int32_t cleanup_failed_load(cs_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                            plugin_context_t* context, managed_component_s*& component,
                            SaoSdkContext*& sdk_context,
                            sao::plugins::sdk_binding::plugin_binding_handle_t& binding,
                            sdk_bridge_session*& sdk_session, int32_t failure_status) noexcept {
    if (owner == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    if (component == nullptr) {
        if (sdk_context != nullptr) {
            const int32_t status = sao_sdk_context_try_destroy(sdk_context);
            if (status != SAO_OK)
                return status;
            sdk_context = nullptr;
        }
        try {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == g_adapter_owner)
                owner->plugins.erase(plugin);
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
        return failure_status;
    }
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner != g_adapter_owner || !owner->active || found == owner->plugins.end())
                return SAO_ERR_HANDLE_INVALID;
            found->second.component = component;
            found->second.context = context;
            found->second.sdk_context = sdk_context;
            found->second.binding = binding;
            found->second.sdk_session = sdk_session;
            found->second.lifecycle = adapter_plugin_record::state::ready;
        }
        component = nullptr;
        sdk_context = nullptr;
        binding = nullptr;
        sdk_session = nullptr;
        const int32_t cleanup_status = adapter_unload(plugin, owner);
        return cleanup_status == SAO_OK ? failure_status : cleanup_status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_load(loader_plugin_handle_t plugin,
                                      const sao::plugins::loader::plugin_manifest* manifest,
                                      void* host_user_data) {
    cs_loader_adapter_owner_s* owner = nullptr;
    bool reserved = false;
    managed_component_s* component = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    sao::plugins::sdk_binding::plugin_binding_handle_t binding = nullptr;
    sdk_bridge_session* sdk_session = nullptr;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            if (plugin == nullptr || manifest == nullptr) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (owner->plugins.find(plugin) != owner->plugins.end()) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            owner->last_errors.erase(plugin);
            owner->plugins.emplace(plugin, adapter_plugin_record{});
            reserved = true;
        }

        plugin_context_t* context = nullptr;
        int32_t status = sao::plugins::loader::sao_plugins_lifecycle_get_context(plugin, &context);
        if (status == SAO_OK && context == nullptr)
            status = SAO_ERR_NOT_INITIALIZED;
        if (status != SAO_OK || context == nullptr) {
            retain_error(owner, plugin, "loader canonical plugin context is unavailable");
        } else {
            std::string error;
            status = cshost_component_load(owner->host, *manifest, &component, error);
            if (status != SAO_OK)
                retain_error(owner, plugin, std::move(error));
        }
        if (status == SAO_OK) {
            status = sao_sdk_context_create(manifest->source_path.c_str(),
                                            manifest->plugin_id.c_str(), &sdk_context);
            if (status == SAO_OK)
                status = sao_sdk_context_bind_platform_services(sdk_context);
            if (status != SAO_OK)
                retain_error(owner, plugin, "C# SDK context initialization failed");
        }
        if (status == SAO_OK) {
            status = cshost_component_attach_contexts(component, sdk_context, context);
            if (status == SAO_OK)
                status = cshost_sdk_bridge_prepare(component, context, sdk_context, component);
            if (status == SAO_OK) {
                status = sao::plugins::sdk_binding::sao_plugins_binding_csharp_activate(
                    reinterpret_cast<sao::plugins::sdk_binding::plugin_context_ptr>(context),
                    reinterpret_cast<sao::plugins::sdk_binding::csharp_domain_ptr>(component),
                    &binding);
            }
            cshost_sdk_bridge_cancel(component);
            if (status == SAO_OK) {
                sdk_session = cshost_sdk_session_find(component);
                status = sdk_session == nullptr
                             ? SAO_ERR_NOT_INITIALIZED
                             : cshost_sdk_bridge_set_binding(sdk_session, binding);
            }
            if (status != SAO_OK)
                retain_error(owner, plugin, "C# SDK binding provider activation failed");
        }
        if (status == SAO_OK) {
            std::string error;
            status = cshost_component_initialize(component, sdk_context, context, error);
            if (status != SAO_OK)
                retain_error(owner, plugin, std::move(error));
        }
        if (status != SAO_OK) {
            reserved = false;
            return cleanup_failed_load(owner, plugin, context, component, sdk_context, binding,
                                       sdk_session, status);
        }

        {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner != g_adapter_owner || !owner->active || found == owner->plugins.end() ||
                found->second.lifecycle != adapter_plugin_record::state::loading) {
                status = SAO_ERR_HANDLE_INVALID;
            } else {
                found->second.component = component;
                found->second.context = context;
                found->second.sdk_context = sdk_context;
                found->second.binding = binding;
                found->second.sdk_session = sdk_session;
                found->second.lifecycle = adapter_plugin_record::state::ready;
                component = nullptr;
                sdk_context = nullptr;
                binding = nullptr;
                sdk_session = nullptr;
                reserved = false;
            }
        }
        if (status != SAO_OK) {
            retain_error(owner, plugin, "managed component load was cancelled");
            return cleanup_failed_load(owner, plugin, context, component, sdk_context, binding,
                                       sdk_session, status);
        }
        return status;
    } catch (...) {
        if (!reserved && component == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        return cleanup_failed_load(owner, plugin, nullptr, component, sdk_context, binding,
                                   sdk_session, SAO_ERR_OS_CALL_FAILED);
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_load(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        plugin_call_lease lease;
        int32_t status = acquire_plugin(host_user_data, plugin, lease);
        if (status != SAO_OK)
            return status;
        int32_t managed_result = 0;
        std::string error;
        status = cshost_component_on_load(lease.component(), &managed_result, error);
        if (status == SAO_OK && managed_result != 0) {
            status = SAO_ERR_OS_CALL_FAILED;
            error = "OnLoad returned " + std::to_string(managed_result);
        }
        if (status != SAO_OK) {
            retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                         error.empty() ? "OnLoad failed" : error);
        }
        if (status == SAO_OK) {
            std::lock_guard lock(g_adapter_mutex);
            auto* owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || found->second.component != lease.component())
                return SAO_ERR_HANDLE_INVALID;
            found->second.on_load_succeeded = true;
        }
        clear_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_enable(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        int32_t managed_result = 0;
        const int32_t status = invoke_hook(plugin, host_user_data, managed_hook::on_enable,
                                           "OnEnable", true, nullptr, 0, &managed_result);
        if (status != SAO_OK)
            return status;
        if (managed_result != 0) {
            retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                         "OnEnable returned " + std::to_string(managed_result));
            return SAO_ERR_OS_CALL_FAILED;
        }
        clear_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_disable(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        int32_t managed_result = 0;
        const int32_t status = invoke_hook(plugin, host_user_data, managed_hook::on_disable,
                                           "OnDisable", true, nullptr, 0, &managed_result);
        if (status != SAO_OK)
            return status;
        if (managed_result != 0) {
            retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                         "OnDisable returned " + std::to_string(managed_result));
            return SAO_ERR_OS_CALL_FAILED;
        }
        clear_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(loader_plugin_handle_t plugin, bool* allow_unload,
                                           void* host_user_data) {
    if (allow_unload == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *allow_unload = false;
    try {
        plugin_call_lease lease;
        int32_t status = acquire_plugin(host_user_data, plugin, lease);
        if (status != SAO_OK)
            return status;
        if (!cshost_component_has_hook(lease.component(), managed_hook::on_unload)) {
            *allow_unload = true;
            clear_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin);
            return SAO_OK;
        }
        int32_t managed_result = 0;
        std::string error;
        status = cshost_component_invoke(lease.component(), managed_hook::on_unload, nullptr, 0,
                                         &managed_result, error);
        if (!lease.on_load_succeeded()) {
            *allow_unload = true;
            if (status != SAO_OK || managed_result != 0) {
                retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                             "OnUnload rollback result ignored: " +
                                 (error.empty() ? std::to_string(managed_result) : error));
            }
            return SAO_OK;
        }
        if (status != SAO_OK) {
            retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                         error.empty() ? "OnUnload failed" : error);
            return status;
        }
        if (managed_result == 0) {
            *allow_unload = true;
            clear_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin);
            return SAO_OK;
        }
        if (managed_result == 1) {
            retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                         "OnUnload vetoed unload");
            return SAO_OK;
        }
        retain_error(static_cast<cs_loader_adapter_owner_s*>(host_user_data), plugin,
                     "OnUnload returned invalid result " + std::to_string(managed_result));
        return SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        cs_loader_adapter_owner_s* owner = nullptr;
        managed_component_s* component = nullptr;
        SaoSdkContext* sdk_context = nullptr;
        sao::plugins::sdk_binding::plugin_binding_handle_t binding = nullptr;
        sdk_bridge_session* sdk_session = nullptr;
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || found->second.component == nullptr) {
                return SAO_ERR_HANDLE_INVALID;
            }
            if (found->second.lifecycle != adapter_plugin_record::state::ready ||
                found->second.active_calls != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second.lifecycle = adapter_plugin_record::state::unloading;
            component = found->second.component;
            sdk_context = found->second.sdk_context;
            binding = found->second.binding;
            sdk_session = found->second.sdk_session;
        }
        if (sdk_session != nullptr) {
            const int32_t status = cshost_sdk_session_quiesce(sdk_session);
            if (status != SAO_OK) {
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end() && found->second.component == component)
                    found->second.lifecycle = adapter_plugin_record::state::ready;
                if (owner == g_adapter_owner && owner->active)
                    owner->last_errors[plugin] = "C# managed SDK session quiesce failed";
                return status;
            }
        }
        if (sdk_context != nullptr) {
            int32_t status = sao_sdk_context_try_destroy(sdk_context);
            if (status == SAO_SDK_ERR_BUSY)
                status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            if (status != SAO_OK) {
                if (sdk_session != nullptr)
                    (void)cshost_sdk_session_resume(sdk_session);
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end() && found->second.component == component)
                    found->second.lifecycle = adapter_plugin_record::state::ready;
                if (owner == g_adapter_owner && owner->active)
                    owner->last_errors[plugin] = "C# SDK context teardown failed";
                return status;
            }
            if (sdk_session != nullptr) {
                (void)cshost_sdk_session_clear_sdk_context(sdk_session, sdk_context);
            }
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found != owner->plugins.end() && found->second.component == component)
                found->second.sdk_context = nullptr;
        }
        if (binding != nullptr) {
            int32_t status = cshost_sdk_session_release_callbacks(sdk_session);
            if (status == SAO_OK) {
                status = sao::plugins::sdk_binding::sao_plugins_binding_csharp_deactivate(binding);
            }
            if (status != SAO_OK) {
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end() && found->second.component == component)
                    found->second.lifecycle = adapter_plugin_record::state::ready;
                if (owner == g_adapter_owner && owner->active)
                    owner->last_errors[plugin] = "C# SDK binding provider teardown failed";
                return status;
            }
            (void)cshost_sdk_bridge_set_binding(sdk_session, nullptr);
            {
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end() && found->second.component == component)
                    found->second.binding = nullptr;
            }
        }
        if (sdk_session != nullptr) {
            const int32_t status = cshost_sdk_bridge_finish(sdk_session);
            {
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end() && found->second.component == component) {
                    if (status == SAO_OK)
                        found->second.sdk_session = nullptr;
                    else
                        found->second.lifecycle = adapter_plugin_record::state::ready;
                }
                if (status != SAO_OK && owner == g_adapter_owner && owner->active)
                    owner->last_errors[plugin] = "C# managed callback release failed";
            }
            if (status != SAO_OK)
                return status;
        }
        std::string error;
        const int32_t close_status = cshost_component_close(component, error);
        if (close_status != SAO_OK) {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found != owner->plugins.end() && found->second.component == component)
                found->second.lifecycle = adapter_plugin_record::state::ready;
            if (owner == g_adapter_owner && owner->active)
                owner->last_errors[plugin] = std::move(error);
            return close_status;
        }
        std::lock_guard lock(g_adapter_mutex);
        const auto found = owner->plugins.find(plugin);
        if (found != owner->plugins.end() && found->second.component == component) {
            owner->plugins.erase(found);
        }
        owner->last_errors.erase(plugin);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sao::plugins::loader::host_adapter_vtable adapter_vtable(cs_loader_adapter_owner_s* owner) {
    sao::plugins::loader::host_adapter_vtable table{};
    table.load_plugin = adapter_load;
    table.call_on_load = adapter_on_load;
    table.call_on_enable = adapter_on_enable;
    table.call_on_disable = adapter_on_disable;
    table.call_on_unload = adapter_on_unload;
    table.unload_plugin = adapter_unload;
    table.host_user_data = owner;
    return table;
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_register_loader_adapter(
    const cs_host_config* host_config, cs_loader_adapter_owner_t* out_owner) {
    if (out_owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard lock(g_adapter_mutex);
        if (g_adapter_owner != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        auto owner = std::make_unique<cs_loader_adapter_owner_s>();
        int32_t status = sao_plugins_cshost_init(host_config, &owner->host);
        if (status != SAO_OK) {
            if (owner->host != nullptr) {
                (void)sao_plugins_cshost_shutdown(owner->host);
                owner->host = nullptr;
            }
            return status;
        }
        status = cshost_register_sdk_binding_provider();
        if (status != SAO_OK) {
            (void)sao_plugins_cshost_shutdown(owner->host);
            return status;
        }
        const auto table = adapter_vtable(owner.get());
        status = sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
            sao::plugins::loader::engine_kind::csharp, &table);
        if (status != SAO_OK) {
            (void)cshost_unregister_sdk_binding_provider();
            (void)sao_plugins_cshost_shutdown(owner->host);
            return status;
        }
        owner->active = true;
        g_adapter_owner = owner.get();
        *out_owner = owner.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_unregister_loader_adapter(cs_loader_adapter_owner_t owner) {
    if (owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(g_adapter_mutex);
        if (owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!owner->plugins.empty()) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        owner->active = false;
        lock.unlock();
        int32_t status = sao::plugins::loader::sao_plugins_lifecycle_unregister_host_adapter(
            sao::plugins::loader::engine_kind::csharp);
        if (status != SAO_OK) {
            lock.lock();
            owner->active = true;
            return status;
        }
        status = cshost_unregister_sdk_binding_provider();
        if (status != SAO_OK) {
            const auto table = adapter_vtable(owner);
            const int32_t restore =
                sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
                    sao::plugins::loader::engine_kind::csharp, &table);
            lock.lock();
            owner->active = restore == SAO_OK;
            return status;
        }
        status = sao_plugins_cshost_shutdown(owner->host);
        if (status != SAO_OK) {
            const int32_t restore_provider = cshost_register_sdk_binding_provider();
            const auto table = adapter_vtable(owner);
            const int32_t restore =
                sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
                    sao::plugins::loader::engine_kind::csharp, &table);
            lock.lock();
            owner->active = restore_provider == SAO_OK && restore == SAO_OK;
            return status;
        }
        lock.lock();
        owner->host = nullptr;
        g_adapter_owner = nullptr;
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_cshost_loader_adapter_plugin_count(cs_loader_adapter_owner_t owner) {
    try {
        std::lock_guard lock(g_adapter_mutex);
        return owner != nullptr && owner == g_adapter_owner && owner->active ? owner->plugins.size()
                                                                             : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_cshost_loader_adapter_get_context(
    cs_loader_adapter_owner_t owner, void* loader_plugin_handle, plugin_context_t** out_context) {
    if (out_context == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_context = nullptr;
    try {
        std::lock_guard lock(g_adapter_mutex);
        if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        const auto found = owner->plugins.find(plugin);
        if (found == owner->plugins.end() || found->second.context == nullptr) {
            return SAO_ERR_HANDLE_INVALID;
        }
        *out_context = found->second.context;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_cshost_loader_adapter_get_last_error(cs_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle, char** out_utf8) {
    if (out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_utf8 = nullptr;
    try {
        std::string error;
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            auto* plugin =
                static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
            const auto found = owner->last_errors.find(plugin);
            if (found != owner->last_errors.end()) {
                error = found->second;
                owner->last_errors.erase(found);
            }
        }
        return cshost_copy_string(error, out_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::csharp_host
