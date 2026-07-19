#include "sao/plugins/emma_host/emma_loader_adapter.h"

#include "sao/plugins/emma_host/emma_call.h"
#include "sao/plugins/emma_host/emma_error.h"
#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace sao::plugins::emma_host {

using loader_plugin_handle_t = sao::plugins::loader::plugin_handle_t;

struct adapter_plugin_record {
    enum class state {
        loading,
        ready,
        unloading,
    };

    emma_plugin_handle_t emma_plugin = nullptr;
    int32_t load_failure_status = SAO_OK;
    state lifecycle = state::loading;
    size_t active_calls = 0;
    std::mutex invocation_mutex;
};

struct emma_loader_adapter_owner_s {
    bool active = false;
    std::unordered_map<loader_plugin_handle_t, std::unique_ptr<adapter_plugin_record>> plugins;
    std::unordered_map<loader_plugin_handle_t, std::string> errors;
};

namespace {

std::mutex g_owner_mutex;
emma_loader_adapter_owner_s* g_owner = nullptr;

emma_loader_adapter_owner_s* active_owner(void* host_user_data) noexcept {
    auto* owner = static_cast<emma_loader_adapter_owner_s*>(host_user_data);
    return owner != nullptr && owner == g_owner && owner->active ? owner : nullptr;
}

void erase_failed_load(emma_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin) noexcept {
    try {
        std::lock_guard lock(g_owner_mutex);
        if (owner == g_owner)
            owner->plugins.erase(plugin);
    } catch (...) {
    }
}

std::string format_error(const emma_error& error, int32_t fallback_status) {
    char* formatted = nullptr;
    if (error.kind != error_kind::none &&
        sao_plugins_emma_error_format(&error, &formatted) == SAO_OK && formatted != nullptr) {
        std::string result(formatted);
        sao_plugins_emma_free_string(formatted);
        return result;
    }
    return "Emma adapter operation failed with status " + std::to_string(fallback_status);
}

void set_last_error(emma_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                    std::string message) {
    std::lock_guard lock(g_owner_mutex);
    if (owner == g_owner && owner->active) {
        if (message.empty()) {
            owner->errors.erase(plugin);
        } else {
            owner->errors[plugin] = std::move(message);
        }
    }
}

void capture_runtime_error(emma_loader_adapter_owner_s* owner, loader_plugin_handle_t loader_plugin,
                           emma_plugin_handle_t emma_plugin, int32_t status) {
    emma_error error;
    if (sao_plugins_emma_get_last_error(emma_plugin, &error) != SAO_OK) {
        error = {};
    }
    set_last_error(owner, loader_plugin, format_error(error, status));
}

class plugin_call_lease {
  public:
    plugin_call_lease() = default;
    ~plugin_call_lease() {
        reset();
    }

    plugin_call_lease(const plugin_call_lease&) = delete;
    plugin_call_lease& operator=(const plugin_call_lease&) = delete;

    emma_plugin_handle_t plugin() const noexcept {
        return emma_plugin_;
    }
    int32_t load_failure_status() const noexcept {
        return record_->load_failure_status;
    }
    std::mutex& invocation_mutex() const noexcept {
        return record_->invocation_mutex;
    }

    void assign(emma_loader_adapter_owner_s* owner, loader_plugin_handle_t loader_plugin,
                adapter_plugin_record* record) noexcept {
        owner_ = owner;
        loader_plugin_ = loader_plugin;
        record_ = record;
        emma_plugin_ = record->emma_plugin;
    }

    void reset() noexcept {
        if (owner_ == nullptr)
            return;
        try {
            std::lock_guard lock(g_owner_mutex);
            const auto found = owner_->plugins.find(loader_plugin_);
            if (found != owner_->plugins.end() && found->second.get() == record_ &&
                record_->active_calls > 0) {
                --record_->active_calls;
            }
        } catch (...) {
        }
        owner_ = nullptr;
        loader_plugin_ = nullptr;
        record_ = nullptr;
        emma_plugin_ = nullptr;
    }

  private:
    emma_loader_adapter_owner_s* owner_ = nullptr;
    loader_plugin_handle_t loader_plugin_ = nullptr;
    adapter_plugin_record* record_ = nullptr;
    emma_plugin_handle_t emma_plugin_ = nullptr;
};

int32_t acquire_plugin(void* host_user_data, loader_plugin_handle_t plugin,
                       plugin_call_lease& lease) {
    std::lock_guard lock(g_owner_mutex);
    auto* owner = active_owner(host_user_data);
    if (owner == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    const auto found = owner->plugins.find(plugin);
    if (found == owner->plugins.end() || found->second == nullptr ||
        found->second->emma_plugin == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second->lifecycle != adapter_plugin_record::state::ready) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second->active_calls;
    lease.assign(owner, plugin, found->second.get());
    return SAO_OK;
}

template <typename Callback>
int32_t with_plugin(loader_plugin_handle_t plugin, void* host_user_data, Callback&& callback) {
    plugin_call_lease lease;
    const int32_t status = acquire_plugin(host_user_data, plugin, lease);
    if (status != SAO_OK)
        return status;
    std::lock_guard invocation_lock(lease.invocation_mutex());
    return callback(lease.plugin());
}

template <typename Callback>
int32_t call_with_error(loader_plugin_handle_t plugin, void* host_user_data, Callback&& callback) {
    auto* owner = static_cast<emma_loader_adapter_owner_s*>(host_user_data);
    return with_plugin(plugin, host_user_data,
                       [owner, plugin, &callback](emma_plugin_handle_t emma_plugin) {
                           const int32_t status = callback(emma_plugin);
                           if (status != SAO_OK) {
                               capture_runtime_error(owner, plugin, emma_plugin, status);
                           }
                           return status;
                       });
}

int32_t SAO_PLUGINS_CALL adapter_load(loader_plugin_handle_t plugin,
                                      const sao::plugins::loader::plugin_manifest* manifest,
                                      void* host_user_data) {
    emma_loader_adapter_owner_s* owner = nullptr;
    emma_plugin_handle_t emma_plugin = nullptr;
    bool load_reserved = false;
    try {
        {
            std::lock_guard lock(g_owner_mutex);
            owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            if (plugin == nullptr || manifest == nullptr || manifest->entry.empty()) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (owner->plugins.find(plugin) != owner->plugins.end()) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            owner->plugins.emplace(plugin, std::make_unique<adapter_plugin_record>());
            owner->errors.erase(plugin);
            load_reserved = true;
        }

        sao::plugins::loader::plugin_context_t* context = nullptr;
        int32_t status = sao::plugins::loader::sao_plugins_lifecycle_get_context(plugin, &context);
        if (status != SAO_OK || context == nullptr) {
            erase_failed_load(owner, plugin);
            return status == SAO_OK ? SAO_ERR_NOT_INITIALIZED : status;
        }

        const wchar_t* plugin_dir = sao::plugins::loader::sao_plugins_ctx_path(context);
        const char* plugin_id = sao::plugins::loader::sao_plugins_ctx_plugin_id(context);
        if (plugin_dir == nullptr || plugin_dir[0] == L'\0' || plugin_id == nullptr ||
            plugin_id[0] == '\0') {
            erase_failed_load(owner, plugin);
            return SAO_ERR_HANDLE_INVALID;
        }
        emma_error load_error;
        status = sao_plugins_emma_load_script_ex(plugin_dir, manifest->entry.c_str(), plugin_id,
                                                 context, &emma_plugin, &load_error);
        if (status != SAO_OK) {
            set_last_error(owner, plugin, format_error(load_error, status));
            if (emma_plugin == nullptr) {
                erase_failed_load(owner, plugin);
                return status;
            }
            std::lock_guard lock(g_owner_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner != g_owner || !owner->active || found == owner->plugins.end() ||
                found->second == nullptr) {
                return status;
            }
            found->second->emma_plugin = emma_plugin;
            found->second->load_failure_status = status;
            found->second->lifecycle = adapter_plugin_record::state::ready;
            emma_plugin = nullptr;
            load_reserved = false;
            return SAO_OK;
        }

        {
            std::lock_guard lock(g_owner_mutex);
            const auto found = owner->plugins.find(plugin);
            if (owner != g_owner || !owner->active || found == owner->plugins.end() ||
                found->second == nullptr ||
                found->second->lifecycle != adapter_plugin_record::state::loading) {
                status = SAO_ERR_HANDLE_INVALID;
            } else {
                found->second->emma_plugin = emma_plugin;
                found->second->lifecycle = adapter_plugin_record::state::ready;
                emma_plugin = nullptr;
                load_reserved = false;
            }
        }
        if (emma_plugin != nullptr) {
            (void)sao_plugins_emma_unload_script(emma_plugin);
        }
        if (status != SAO_OK)
            erase_failed_load(owner, plugin);
        return status;
    } catch (...) {
        if (emma_plugin != nullptr) {
            (void)sao_plugins_emma_unload_script(emma_plugin);
        }
        if (load_reserved && owner != nullptr)
            erase_failed_load(owner, plugin);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_load(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        plugin_call_lease lease;
        const int32_t acquire_status = acquire_plugin(host_user_data, plugin, lease);
        if (acquire_status != SAO_OK)
            return acquire_status;
        if (lease.load_failure_status() != SAO_OK)
            return lease.load_failure_status();
        std::lock_guard invocation_lock(lease.invocation_mutex());
        const int32_t status = sao_plugins_emma_call_on_load(lease.plugin());
        if (status != SAO_OK) {
            capture_runtime_error(static_cast<emma_loader_adapter_owner_s*>(host_user_data), plugin,
                                  lease.plugin(), status);
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_enable(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        return call_with_error(plugin, host_user_data, [](emma_plugin_handle_t emma_plugin) {
            return sao_plugins_emma_call_on_enable(emma_plugin);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_disable(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        return call_with_error(plugin, host_user_data, [](emma_plugin_handle_t emma_plugin) {
            return sao_plugins_emma_call_on_disable(emma_plugin);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(loader_plugin_handle_t plugin, bool* out_allow_unload,
                                           void* host_user_data) {
    if (out_allow_unload != nullptr)
        *out_allow_unload = true;
    try {
        {
            std::lock_guard lock(g_owner_mutex);
            auto* owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found != owner->plugins.end() && found->second != nullptr &&
                found->second->load_failure_status != SAO_OK) {
                return SAO_OK;
            }
        }
        return call_with_error(
            plugin, host_user_data, [out_allow_unload](emma_plugin_handle_t emma_plugin) {
                return sao_plugins_emma_call_on_unload(emma_plugin, out_allow_unload);
            });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin, void* host_user_data) {
    try {
        emma_loader_adapter_owner_s* owner = nullptr;
        emma_plugin_handle_t emma_plugin = nullptr;
        {
            std::lock_guard lock(g_owner_mutex);
            owner = active_owner(host_user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || found->second == nullptr ||
                found->second->emma_plugin == nullptr) {
                return SAO_ERR_HANDLE_INVALID;
            }
            if ((found->second->lifecycle != adapter_plugin_record::state::ready &&
                 found->second->lifecycle != adapter_plugin_record::state::unloading) ||
                found->second->active_calls != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second->lifecycle = adapter_plugin_record::state::unloading;
            emma_plugin = found->second->emma_plugin;
        }

        const int32_t status = sao_plugins_emma_unload_script(emma_plugin);
        std::lock_guard lock(g_owner_mutex);
        const auto found = owner->plugins.find(plugin);
        if (status != SAO_OK) {
            if (found != owner->plugins.end() && found->second != nullptr) {
                found->second->lifecycle = adapter_plugin_record::state::unloading;
            }
            return status;
        }
        if (found != owner->plugins.end())
            owner->plugins.erase(found);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sao::plugins::loader::host_adapter_vtable
adapter_vtable(emma_loader_adapter_owner_s* owner) noexcept {
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
sao_plugins_emma_register_loader_adapter(emma_loader_adapter_owner_t* out_owner) {
    if (out_owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
    try {
        std::lock_guard lock(g_owner_mutex);
        if (g_owner != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        auto owner = std::make_unique<emma_loader_adapter_owner_s>();
        const auto table = adapter_vtable(owner.get());
        const int32_t status = sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
            sao::plugins::loader::engine_kind::emma, &table);
        if (status != SAO_OK)
            return status;
        owner->active = true;
        g_owner = owner.get();
        *out_owner = owner.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_emma_unregister_loader_adapter(emma_loader_adapter_owner_t owner) {
    if (owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::unique_lock lock(g_owner_mutex);
        if (owner != g_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!owner->plugins.empty()) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        const int32_t status = sao::plugins::loader::sao_plugins_lifecycle_unregister_host_adapter(
            sao::plugins::loader::engine_kind::emma);
        if (status != SAO_OK)
            return status;
        owner->active = false;
        g_owner = nullptr;
        lock.unlock();
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_emma_loader_adapter_plugin_count(emma_loader_adapter_owner_t owner) {
    try {
        std::lock_guard lock(g_owner_mutex);
        return owner != nullptr && owner == g_owner && owner->active ? owner->plugins.size() : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_emma_loader_adapter_get_last_error(
    emma_loader_adapter_owner_t owner, void* loader_plugin_handle, char** out_utf8) {
    if (out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_utf8 = nullptr;
    if (owner == nullptr || loader_plugin_handle == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard lock(g_owner_mutex);
        if (owner != g_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        const auto found =
            owner->errors.find(static_cast<loader_plugin_handle_t>(loader_plugin_handle));
        if (found == owner->errors.end())
            return SAO_ERR_HANDLE_INVALID;
        auto* copy = static_cast<char*>(std::malloc(found->second.size() + 1));
        if (copy == nullptr)
            return SAO_ERR_OS_CALL_FAILED;
        std::memcpy(copy, found->second.c_str(), found->second.size() + 1);
        *out_utf8 = copy;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::emma_host
