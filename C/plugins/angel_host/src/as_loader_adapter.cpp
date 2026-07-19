#include "sao/plugins/angel_host/as_call.h"

#include "as_plugin_internal.h"

#include "sao/plugins/angel_host/as_gpu_hunt_bind.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/sdk/sao_sdk.h"
#include "sao/sdk/sao_sdk_provider.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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
};

struct adapter_resource_snapshot {
    as_host_handle_t host = nullptr;
    as_plugin_handle_t script = nullptr;
    SaoSdkContext* sdk_context = nullptr;
    sao::plugins::loader::plugin_context_t* loader_context = nullptr;
};

struct as_loader_adapter_owner_s {
    as_host_config config{};
    bool active = false;
    std::unordered_map<loader_plugin_handle_t, adapter_plugin_record> plugins;
    std::unordered_map<loader_plugin_handle_t, std::string> last_errors;
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
    return owner != nullptr && owner == g_adapter_owner && owner->active ? owner : nullptr;
}

int32_t copy_string(const std::string& value, char** output) {
    if (output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    auto* copy = static_cast<char*>(std::malloc(value.size() + 1));
    if (copy == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    std::memcpy(copy, value.c_str(), value.size() + 1);
    *output = copy;
    return SAO_OK;
}

void retain_script_error(as_loader_adapter_owner_s* owner, loader_plugin_handle_t loader_plugin,
                         as_plugin_handle_t script, const char* phase, const char* fallback) {
    const std::string error = plugin_error_text(script, phase, fallback);
    std::lock_guard lock(g_adapter_mutex);
    if (owner == g_adapter_owner && owner->active) {
        owner->last_errors[loader_plugin] = error;
    }
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(__cpp_char8_t)
    std::u8string encoded;
    while (*value != '\0') {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(*value++)));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::u8path(value);
#endif
}

int32_t destroy_snapshot(adapter_resource_snapshot& snapshot) noexcept {
    if (snapshot.script != nullptr) {
        try {
            const int32_t status = sao_plugins_ashost_unload_script(snapshot.script);
            if (status != SAO_OK)
                return status;
            snapshot.script = nullptr;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    if (snapshot.host != nullptr) {
        try {
            const int32_t status = sao_plugins_ashost_destroy(snapshot.host);
            if (status != SAO_OK)
                return status;
            snapshot.host = nullptr;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    if (snapshot.sdk_context != nullptr) {
        try {
            int32_t status = sao_sdk_context_try_destroy(snapshot.sdk_context);
            if (status == SAO_SDK_ERR_BUSY)
                status = sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            if (status != SAO_OK)
                return status;
            snapshot.sdk_context = nullptr;
        } catch (...) {
            return SAO_ERR_OS_CALL_FAILED;
        }
    }
    snapshot.loader_context = nullptr;
    return SAO_OK;
}

adapter_resource_snapshot take_resources(adapter_plugin_record& record) noexcept {
    adapter_resource_snapshot snapshot{record.host, record.script, record.sdk_context,
                                       record.loader_context};
    record.host = nullptr;
    record.script = nullptr;
    record.sdk_context = nullptr;
    record.loader_context = nullptr;
    return snapshot;
}

void restore_resources(adapter_plugin_record& record,
                       adapter_resource_snapshot& snapshot) noexcept {
    record.host = std::exchange(snapshot.host, nullptr);
    record.script = std::exchange(snapshot.script, nullptr);
    record.sdk_context = std::exchange(snapshot.sdk_context, nullptr);
    record.loader_context = std::exchange(snapshot.loader_context, nullptr);
}

class plugin_call_lease {
  public:
    ~plugin_call_lease() noexcept {
        reset();
    }

    plugin_call_lease(const plugin_call_lease&) = delete;
    plugin_call_lease& operator=(const plugin_call_lease&) = delete;
    plugin_call_lease() = default;

    as_plugin_handle_t script() const {
        return script_;
    }

    void assign(as_loader_adapter_owner_s* owner, loader_plugin_handle_t loader_plugin,
                as_plugin_handle_t script) {
        owner_ = owner;
        loader_plugin_ = loader_plugin;
        script_ = script;
    }

  private:
    void reset() noexcept {
        if (owner_ == nullptr)
            return;
        try {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner_->plugins.find(loader_plugin_);
            if (found != owner_->plugins.end() && found->second.script == script_ &&
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

int32_t acquire_plugin(void* user_data, loader_plugin_handle_t plugin, plugin_call_lease& lease) {
    std::lock_guard lock(g_adapter_mutex);
    auto* owner = active_owner(user_data);
    if (owner == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    const auto found = owner->plugins.find(plugin);
    if (found == owner->plugins.end()) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second.lifecycle != adapter_plugin_record::state::ready) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    if (found->second.script == nullptr)
        return SAO_ERR_HANDLE_INVALID;
    ++found->second.active_calls;
    lease.assign(owner, plugin, found->second.script);
    return SAO_OK;
}

template <typename Callback>
int32_t with_plugin(loader_plugin_handle_t plugin, void* user_data, Callback&& callback) {
    plugin_call_lease lease;
    const int32_t status = acquire_plugin(user_data, plugin, lease);
    return status == SAO_OK ? callback(lease.script()) : status;
}

int32_t SAO_PLUGINS_CALL adapter_load(loader_plugin_handle_t plugin,
                                      const sao::plugins::loader::plugin_manifest* manifest,
                                      void* user_data) {
    as_loader_adapter_owner_s* owner = nullptr;
    adapter_plugin_record pending;
    bool reserved = false;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            if (plugin == nullptr || manifest == nullptr || manifest->plugin_id.empty() ||
                manifest->source_path.empty()) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (owner->plugins.find(plugin) != owner->plugins.end()) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            owner->last_errors.erase(plugin);
            owner->plugins.emplace(plugin, adapter_plugin_record{});
            reserved = true;
        }

        int32_t status = sao::plugins::loader::sao_plugins_lifecycle_get_context(
            plugin, &pending.loader_context);
        if (status != SAO_OK || pending.loader_context == nullptr) {
            if (status == SAO_OK)
                status = SAO_ERR_NOT_INITIALIZED;
        } else {
            status = sao_plugins_ashost_create(&owner->config, &pending.host);
        }
        if (status == SAO_OK) {
            status = sao_sdk_context_create(manifest->source_path.c_str(),
                                            manifest->plugin_id.c_str(), &pending.sdk_context);
        }
        if (status == SAO_OK) {
            status = sao_sdk_context_bind_platform_services(pending.sdk_context);
        }
        asIScriptEngine* engine = nullptr;
        if (status == SAO_OK) {
            engine = sao_plugins_ashost_engine(pending.host);
            if (engine == nullptr)
                status = SAO_ERR_HANDLE_INVALID;
        }
        if (status == SAO_OK) {
            status = register_gpu_hunt_bindings(engine, pending.sdk_context);
        }
        if (status == SAO_OK) {
            const std::wstring plugin_dir = path_from_utf8(manifest->source_path.c_str()).wstring();
            const std::string entry =
                manifest->entry.empty() ? std::string("main.as") : manifest->entry;
            status = sao_plugins_ashost_load_script(engine, plugin_dir.c_str(), entry.c_str(),
                                                    manifest->plugin_id.c_str(),
                                                    pending.loader_context, &pending.script);
        }
        if (status != SAO_OK) {
            std::string retained_error = "AngelScript plugin runtime load failed";
            if (pending.host != nullptr) {
                char* host_error = nullptr;
                if (sao_plugins_ashost_get_last_error(pending.host, &host_error) == SAO_OK &&
                    host_error != nullptr) {
                    retained_error = host_error;
                }
                std::free(host_error);
            }
            adapter_resource_snapshot snapshot = take_resources(pending);
            const int32_t cleanup_status = destroy_snapshot(snapshot);
            std::lock_guard lock(g_adapter_mutex);
            if (owner == g_adapter_owner) {
                owner->plugins.erase(plugin);
                owner->last_errors[plugin] = std::move(retained_error);
            }
            return cleanup_status == SAO_OK ? status : cleanup_status;
        }

        {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner != g_adapter_owner || !owner->active || found == owner->plugins.end() ||
                found->second.lifecycle != adapter_plugin_record::state::loading) {
                status = SAO_ERR_HANDLE_INVALID;
            } else {
                pending.lifecycle = adapter_plugin_record::state::ready;
                found->second = std::move(pending);
                reserved = false;
            }
        }
        if (status != SAO_OK) {
            adapter_resource_snapshot snapshot = take_resources(pending);
            const int32_t cleanup_status = destroy_snapshot(snapshot);
            if (cleanup_status != SAO_OK)
                status = cleanup_status;
        }
        return status;
    } catch (...) {
        adapter_resource_snapshot snapshot = take_resources(pending);
        const int32_t cleanup_status = destroy_snapshot(snapshot);
        if (reserved && owner != nullptr) {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == g_adapter_owner)
                owner->plugins.erase(plugin);
        }
        return cleanup_status == SAO_OK ? SAO_ERR_OS_CALL_FAILED : cleanup_status;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_load(loader_plugin_handle_t plugin, void* user_data) {
    try {
        auto* owner = static_cast<as_loader_adapter_owner_s*>(user_data);
        return with_plugin(plugin, user_data, [owner, plugin](as_plugin_handle_t script) {
            const int32_t status = sao_plugins_ashost_call_on_load(script);
            if (status != SAO_OK) {
                retain_script_error(owner, plugin, script, "on_load", "AngelScript on_load failed");
            }
            return status;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_enable(loader_plugin_handle_t plugin, void* user_data) {
    try {
        auto* owner = static_cast<as_loader_adapter_owner_s*>(user_data);
        return with_plugin(plugin, user_data, [owner, plugin](as_plugin_handle_t script) {
            const int32_t status = sao_plugins_ashost_call_on_enable(script);
            if (status != SAO_OK)
                retain_script_error(owner, plugin, script, "on_enable",
                                    "AngelScript on_enable failed");
            return status;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_disable(loader_plugin_handle_t plugin, void* user_data) {
    try {
        auto* owner = static_cast<as_loader_adapter_owner_s*>(user_data);
        return with_plugin(plugin, user_data, [owner, plugin](as_plugin_handle_t script) {
            const int32_t status = sao_plugins_ashost_call_on_disable(script);
            if (status != SAO_OK)
                retain_script_error(owner, plugin, script, "on_disable",
                                    "AngelScript on_disable failed");
            return status;
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(loader_plugin_handle_t plugin, bool* allow_unload,
                                           void* user_data) {
    if (allow_unload == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *allow_unload = true;
    try {
        auto* owner = static_cast<as_loader_adapter_owner_s*>(user_data);
        return with_plugin(
            plugin, user_data, [owner, plugin, allow_unload](as_plugin_handle_t script) {
                const int32_t status = sao_plugins_ashost_call_on_unload(script, allow_unload);
                if (status != SAO_OK)
                    retain_script_error(owner, plugin, script, "on_unload",
                                        "AngelScript on_unload failed");
                return status;
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin, void* user_data) {
    try {
        as_loader_adapter_owner_s* owner = nullptr;
        adapter_resource_snapshot snapshot;
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end())
                return SAO_ERR_HANDLE_INVALID;
            if (found->second.lifecycle != adapter_plugin_record::state::ready) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            if (found->second.active_calls != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second.lifecycle = adapter_plugin_record::state::unloading;
            snapshot = take_resources(found->second);
        }
        const int32_t status = destroy_snapshot(snapshot);
        if (status != SAO_OK) {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner == g_adapter_owner && found != owner->plugins.end() &&
                found->second.lifecycle == adapter_plugin_record::state::unloading) {
                restore_resources(found->second, snapshot);
                found->second.lifecycle = adapter_plugin_record::state::ready;
            }
            return status;
        }
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner != g_adapter_owner)
                return SAO_ERR_HANDLE_INVALID;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() ||
                found->second.lifecycle != adapter_plugin_record::state::unloading) {
                return SAO_ERR_HANDLE_INVALID;
            }
            owner->plugins.erase(found);
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sao::plugins::loader::host_adapter_vtable adapter_vtable(as_loader_adapter_owner_s* owner) {
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

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_ashost_register_loader_adapter(
    const as_host_config* cfg, as_loader_adapter_owner_t* out_owner) {
    if (out_owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard lock(g_adapter_mutex);
        if (g_adapter_owner != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        auto owner = std::make_unique<as_loader_adapter_owner_s>();
        if (cfg != nullptr)
            owner->config = *cfg;
        owner->active = true;
        const auto table = adapter_vtable(owner.get());
        const int32_t status = sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
            sao::plugins::loader::engine_kind::angelscript, &table);
        if (status != SAO_OK)
            return status;
        g_adapter_owner = owner.get();
        *out_owner = owner.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_unregister_loader_adapter(as_loader_adapter_owner_t owner) {
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
        const int32_t status = sao::plugins::loader::sao_plugins_lifecycle_unregister_host_adapter(
            sao::plugins::loader::engine_kind::angelscript);
        if (status != SAO_OK) {
            lock.lock();
            if (owner == g_adapter_owner) {
                owner->active = true;
            }
            return status;
        }
        lock.lock();
        g_adapter_owner = nullptr;
        lock.unlock();
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_plugin_count(as_loader_adapter_owner_t owner) {
    try {
        std::lock_guard lock(g_adapter_mutex);
        return owner != nullptr && owner == g_adapter_owner && owner->active ? owner->plugins.size()
                                                                             : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_get_last_error(as_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle, char** out_utf8) {
    if (out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_utf8 = nullptr;
    if (owner == nullptr || loader_plugin_handle == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        std::string error;
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner != g_adapter_owner || !owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            const auto found = owner->last_errors.find(plugin);
            if (found == owner->last_errors.end()) {
                return SAO_ERR_HANDLE_INVALID;
            }
            error = found->second;
        }
        return copy_string(error, out_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_acquire_plugin(as_loader_adapter_owner_t owner,
                                                 void* loader_plugin_handle,
                                                 as_loader_adapter_plugin_lease_t* out_lease) {
    if (out_lease == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_lease = nullptr;
    try {
        auto lease = std::make_unique<as_loader_adapter_plugin_lease_s>();
        std::lock_guard lock(g_adapter_mutex);
        if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        const auto found = owner->plugins.find(plugin);
        if (found == owner->plugins.end()) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (found->second.lifecycle != adapter_plugin_record::state::ready)
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        if (found->second.script == nullptr || found->second.sdk_context == nullptr)
            return SAO_ERR_HANDLE_INVALID;
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
sao_plugins_ashost_loader_adapter_lease_script(as_loader_adapter_plugin_lease_t lease) {
    return lease != nullptr && lease->active ? lease->script : nullptr;
}

extern "C" SAO_PLUGINS_API SaoSdkContext* SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_lease_sdk_context(as_loader_adapter_plugin_lease_t lease) {
    return lease != nullptr && lease->active ? lease->sdk_context : nullptr;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_ashost_loader_adapter_release_plugin(as_loader_adapter_plugin_lease_t lease) {
    if (lease == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            if (!lease->active || lease->owner == nullptr || lease->owner != g_adapter_owner ||
                !lease->owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            const auto found = lease->owner->plugins.find(lease->loader_plugin);
            if (found == lease->owner->plugins.end() || found->second.script != lease->script ||
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
