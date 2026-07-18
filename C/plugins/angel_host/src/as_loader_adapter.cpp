#include "sao/plugins/angel_host/as_call.h"

#include "sao/plugins/angel_host/as_gpu_hunt_bind.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace sao::plugins::angel_host {

using loader_plugin_handle_t = sao::plugins::loader::plugin_handle_t;

struct adapter_plugin_record {
    enum class state {
        loading,
        ready,
        unloading,
    } lifecycle = state::loading;
    as_host_handle_t host = nullptr;
    as_plugin_handle_t script = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    sao::plugins::loader::plugin_context_t* loader_context = nullptr;
    size_t active_calls = 0;
    bool cleanup_active = false;
};

struct as_loader_adapter_owner_s {
    as_host_config config{};
    bool active = false;
    std::unordered_map<loader_plugin_handle_t, adapter_plugin_record> plugins;
};

struct as_loader_adapter_plugin_lease_s {
    as_loader_adapter_owner_s* owner = nullptr;
    loader_plugin_handle_t loader_plugin = nullptr;
    as_plugin_handle_t script = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    bool active = false;
};

namespace {

std::mutex g_adapter_mutex;
as_loader_adapter_owner_s* g_adapter_owner = nullptr;

as_loader_adapter_owner_s* active_owner(void* user_data) {
    auto* owner = static_cast<as_loader_adapter_owner_s*>(user_data);
    return owner != nullptr && owner == g_adapter_owner && owner->active
               ? owner
               : nullptr;
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(__cpp_char8_t)
    std::u8string encoded;
    while (*value != '\0') {
        encoded.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(*value++)));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::u8path(value);
#endif
}

int32_t destroy_record(adapter_plugin_record& record) noexcept {
    if (record.script != nullptr) {
        try {
            const int32_t status =
                sao_plugins_ashost_unload_script(record.script);
            if (status != SAO_OK) return status;
            record.script = nullptr;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    if (record.host != nullptr) {
        try {
            const int32_t status = sao_plugins_ashost_destroy(record.host);
            if (status != SAO_OK) return status;
            record.host = nullptr;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    if (record.sdk_context != nullptr) {
        try {
            sao_sdk_context_destroy(record.sdk_context);
            record.sdk_context = nullptr;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    record.loader_context = nullptr;
    return SAO_OK;
}

class plugin_call_lease {
public:
    ~plugin_call_lease() noexcept { reset(); }

    plugin_call_lease(const plugin_call_lease&) = delete;
    plugin_call_lease& operator=(const plugin_call_lease&) = delete;
    plugin_call_lease() = default;

    as_plugin_handle_t script() const { return script_; }

    void assign(as_loader_adapter_owner_s* owner,
                loader_plugin_handle_t loader_plugin,
                as_plugin_handle_t script) {
        owner_ = owner;
        loader_plugin_ = loader_plugin;
        script_ = script;
    }

private:
    void reset() noexcept {
        if (owner_ == nullptr) return;
        try {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner_->plugins.find(loader_plugin_);
            if (found != owner_->plugins.end() &&
                found->second.script == script_ &&
                found->second.active_calls != 0) {
                --found->second.active_calls;
            }
        } catch (...) {
        }
        owner_ = nullptr;
        loader_plugin_ = nullptr;
        script_ = nullptr;
    }

    as_loader_adapter_owner_s* owner_ = nullptr;
    loader_plugin_handle_t loader_plugin_ = nullptr;
    as_plugin_handle_t script_ = nullptr;
};

int32_t acquire_plugin(void* user_data,
                       loader_plugin_handle_t plugin,
                       plugin_call_lease& lease) {
    std::lock_guard lock(g_adapter_mutex);
    auto* owner = active_owner(user_data);
    if (owner == nullptr) return SAO_ERR_NOT_INITIALIZED;
    const auto found = owner->plugins.find(plugin);
    if (found == owner->plugins.end() || found->second.script == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second.lifecycle != adapter_plugin_record::state::ready) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second.active_calls;
    lease.assign(owner, plugin, found->second.script);
    return SAO_OK;
}

template <typename Callback>
int32_t with_plugin(loader_plugin_handle_t plugin,
                    void* user_data,
                    Callback&& callback) {
    plugin_call_lease lease;
    const int32_t status = acquire_plugin(user_data, plugin, lease);
    return status == SAO_OK ? callback(lease.script()) : status;
}

int32_t SAO_PLUGINS_CALL adapter_load(
    loader_plugin_handle_t plugin,
    const sao::plugins::loader::plugin_manifest* manifest,
    void* user_data) {
    as_loader_adapter_owner_s* owner = nullptr;
    adapter_plugin_record pending;
    bool reserved = false;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr) return SAO_ERR_NOT_INITIALIZED;
            if (plugin == nullptr || manifest == nullptr ||
                manifest->plugin_id.empty() || manifest->source_path.empty()) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (owner->plugins.find(plugin) != owner->plugins.end()) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            owner->plugins.emplace(plugin, adapter_plugin_record{});
            reserved = true;
        }

        int32_t status =
            sao::plugins::loader::sao_plugins_lifecycle_get_context(
                plugin, &pending.loader_context);
        if (status != SAO_OK || pending.loader_context == nullptr) {
            if (status == SAO_OK) status = SAO_ERR_NOT_INITIALIZED;
        } else {
            status = sao_plugins_ashost_create(&owner->config, &pending.host);
        }
        if (status == SAO_OK) {
            status = sao_sdk_context_create(manifest->source_path.c_str(),
                                            manifest->plugin_id.c_str(),
                                            &pending.sdk_context);
        }
        if (status == SAO_OK) {
            status = sao_sdk_context_bind_platform_services(pending.sdk_context);
        }
        asIScriptEngine* engine = nullptr;
        if (status == SAO_OK) {
            engine = sao_plugins_ashost_engine(pending.host);
            if (engine == nullptr) status = SAO_ERR_HANDLE_INVALID;
        }
        if (status == SAO_OK) {
            status = register_gpu_hunt_bindings(engine, pending.sdk_context);
        }
        if (status == SAO_OK) {
            const std::wstring plugin_dir =
                path_from_utf8(manifest->source_path.c_str()).wstring();
            const std::string entry = manifest->entry.empty()
                                          ? std::string("main.as")
                                          : manifest->entry;
            status = sao_plugins_ashost_load_script(
                engine, plugin_dir.c_str(), entry.c_str(),
                manifest->plugin_id.c_str(), pending.loader_context,
                &pending.script);
        }
        if (status != SAO_OK) {
            const int32_t cleanup_status = destroy_record(pending);
            std::lock_guard lock(g_adapter_mutex);
            if (owner == g_adapter_owner) owner->plugins.erase(plugin);
            return cleanup_status == SAO_OK ? status : cleanup_status;
        }

        {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner != g_adapter_owner || !owner->active ||
                found == owner->plugins.end() ||
                found->second.lifecycle !=
                    adapter_plugin_record::state::loading) {
                status = SAO_ERR_HANDLE_INVALID;
            } else {
                pending.lifecycle = adapter_plugin_record::state::ready;
                found->second = std::move(pending);
                reserved = false;
            }
        }
        if (status != SAO_OK) {
            const int32_t cleanup_status = destroy_record(pending);
            if (cleanup_status != SAO_OK) status = cleanup_status;
        }
        return status;
    } catch (...) {
        const int32_t cleanup_status = destroy_record(pending);
        if (reserved && owner != nullptr) {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == g_adapter_owner) owner->plugins.erase(plugin);
        }
        return cleanup_status == SAO_OK ? SAO_ERR_OS_CALL_FAILED
                                        : cleanup_status;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_load(loader_plugin_handle_t plugin,
                                         void* user_data) {
    try {
        return with_plugin(plugin, user_data, [](as_plugin_handle_t script) {
            return sao_plugins_ashost_call_on_load(script);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_enable(loader_plugin_handle_t plugin,
                                           void* user_data) {
    try {
        return with_plugin(plugin, user_data, [](as_plugin_handle_t script) {
            return sao_plugins_ashost_call_on_enable(script);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_disable(loader_plugin_handle_t plugin,
                                            void* user_data) {
    try {
        return with_plugin(plugin, user_data, [](as_plugin_handle_t script) {
            return sao_plugins_ashost_call_on_disable(script);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(loader_plugin_handle_t plugin,
                                           bool* allow_unload,
                                           void* user_data) {
    if (allow_unload == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *allow_unload = true;
    try {
        return with_plugin(
            plugin, user_data, [allow_unload](as_plugin_handle_t script) {
                return sao_plugins_ashost_call_on_unload(script, allow_unload);
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin,
                                        void* user_data) {
    try {
        as_loader_adapter_owner_s* owner = nullptr;
        adapter_plugin_record* record = nullptr;
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr) return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end()) return SAO_ERR_HANDLE_INVALID;
            if (found->second.active_calls != 0 ||
                found->second.cleanup_active ||
                (found->second.lifecycle !=
                     adapter_plugin_record::state::ready &&
                 found->second.lifecycle !=
                     adapter_plugin_record::state::unloading)) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second.lifecycle = adapter_plugin_record::state::unloading;
            found->second.cleanup_active = true;
            record = &found->second;
        }
        const int32_t status = destroy_record(*record);
        if (status != SAO_OK) {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found != owner->plugins.end() && &found->second == record) {
                found->second.cleanup_active = false;
            }
            return status;
        }
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner != g_adapter_owner) return SAO_ERR_HANDLE_INVALID;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || &found->second != record) {
                return SAO_ERR_HANDLE_INVALID;
            }
            owner->plugins.erase(found);
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sao::plugins::loader::host_adapter_vtable adapter_vtable(
    as_loader_adapter_owner_s* owner) {
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

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_register_loader_adapter(
    const as_host_config* cfg,
    as_loader_adapter_owner_t* out_owner) {
    if (out_owner == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard lock(g_adapter_mutex);
        if (g_adapter_owner != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        auto owner = std::make_unique<as_loader_adapter_owner_s>();
        if (cfg != nullptr) owner->config = *cfg;
        owner->active = true;
        const auto table = adapter_vtable(owner.get());
        const int32_t status =
            sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
                sao::plugins::loader::engine_kind::angelscript, &table);
        if (status != SAO_OK) return status;
        g_adapter_owner = owner.get();
        *out_owner = owner.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unregister_loader_adapter(
    as_loader_adapter_owner_t owner) {
    if (owner == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(g_adapter_mutex);
        if (owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!owner->plugins.empty()) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        lock.unlock();
        const int32_t status =
            sao::plugins::loader::sao_plugins_lifecycle_unregister_host_adapter(
                sao::plugins::loader::engine_kind::angelscript);
        if (status != SAO_OK) return status;
        lock.lock();
        owner->active = false;
        g_adapter_owner = nullptr;
        lock.unlock();
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_plugin_count(
    as_loader_adapter_owner_t owner) {
    try {
        std::lock_guard lock(g_adapter_mutex);
        return owner != nullptr && owner == g_adapter_owner && owner->active
                   ? owner->plugins.size()
                   : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_acquire_plugin(
    as_loader_adapter_owner_t owner,
    void* loader_plugin_handle,
    as_loader_adapter_plugin_lease_t* out_lease) {
    if (out_lease == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_lease = nullptr;
    try {
        auto lease = std::make_unique<as_loader_adapter_plugin_lease_s>();
        std::lock_guard lock(g_adapter_mutex);
        if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(
            loader_plugin_handle);
        const auto found = owner->plugins.find(plugin);
        if (found == owner->plugins.end() || found->second.script == nullptr ||
            found->second.sdk_context == nullptr ||
            found->second.lifecycle != adapter_plugin_record::state::ready) {
            return SAO_ERR_HANDLE_INVALID;
        }
        ++found->second.active_calls;
        lease->owner = owner;
        lease->loader_plugin = plugin;
        lease->script = found->second.script;
        lease->sdk_context = found->second.sdk_context;
        lease->active = true;
        *out_lease = lease.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API as_plugin_handle_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_lease_script(
    as_loader_adapter_plugin_lease_t lease) {
    return lease != nullptr && lease->active ? lease->script : nullptr;
}

extern "C" SAO_PLUGINS_API SaoSdkContext* SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_lease_sdk_context(
    as_loader_adapter_plugin_lease_t lease) {
    return lease != nullptr && lease->active ? lease->sdk_context : nullptr;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_release_plugin(
    as_loader_adapter_plugin_lease_t lease) {
    if (lease == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            if (!lease->active || lease->owner == nullptr ||
                lease->owner != g_adapter_owner || !lease->owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            const auto found =
                lease->owner->plugins.find(lease->loader_plugin);
            if (found == lease->owner->plugins.end() ||
                found->second.script != lease->script ||
                found->second.sdk_context != lease->sdk_context ||
                found->second.active_calls == 0) {
                return SAO_ERR_HANDLE_INVALID;
            }
            --found->second.active_calls;
            lease->active = false;
        }
        delete lease;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::angel_host
