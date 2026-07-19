#include "sao/plugins/lua_host/lua_host.h"

#include "sao/plugins/loader/loader_status.h"
#include "sao/plugins/loader/plugin_context.h"
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/lua_host/lua_call.h"
#include "sao/plugins/lua_host/lua_error.h"
#include "sao/plugins/lua_host/lua_sandbox.h"
#include "sao/plugins/lua_host/lua_stdlib.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(SAO_HAS_LUA)
#include "lua_bridge_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

using loader_plugin_handle_t = sao::plugins::loader::plugin_handle_t;

struct adapter_plugin_record {
    lua_host_handle_t host = nullptr;
    lua_plugin_handle_t plugin = nullptr;
    enum class state {
        loading,
        ready,
        unloading,
        load_cleanup_pending,
        host_destroy_pending,
    } lifecycle = state::loading;
    size_t active_calls = 0;
};

struct lua_loader_adapter_owner_s {
    lua_host_config config{};
    bool active = false;
    std::unordered_map<loader_plugin_handle_t, adapter_plugin_record> plugins;
    std::unordered_map<loader_plugin_handle_t, std::string> last_errors;
};

namespace {

std::mutex g_adapter_mutex;
lua_loader_adapter_owner_s* g_adapter_owner = nullptr;

bool has_permission(const sao::plugins::loader::plugin_manifest& manifest, const char* permission) {
    for (const auto& value : manifest.permissions) {
        if (value == permission)
            return true;
    }
    return false;
}

bool utf8_to_wide(const std::string& value, std::wstring& output) {
    output.clear();
    if (value.empty())
        return false;
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0)
        return false;
    output.resize(static_cast<size_t>(needed));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), output.data(), needed) == needed;
}

int32_t copy_string(const std::string& value, char** output) {
    if (output == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    auto* copy = static_cast<char*>(std::malloc(value.size() + 1));
    if (copy == nullptr)
        return SAO_ERR_OS_CALL_FAILED;
    if (!value.empty())
        std::memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    *output = copy;
    return SAO_OK;
}

lua_loader_adapter_owner_s* active_owner(void* user_data) {
    auto* owner = static_cast<lua_loader_adapter_owner_s*>(user_data);
    return owner != nullptr && owner == g_adapter_owner && owner->active ? owner : nullptr;
}

void retain_error(lua_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                  std::string error) {
    std::lock_guard lock(g_adapter_mutex);
    if (owner == g_adapter_owner && owner->active) {
        owner->last_errors[plugin] = std::move(error);
    }
}

std::string take_state_error(lua_host_handle_t host) {
    if (host == nullptr)
        return {};
    lua_State* state = sao_plugins_luahost_state(host);
    if (state == nullptr)
        return {};
    char* error = nullptr;
    if (sao_plugins_luahost_take_error(state, &error) != SAO_OK || error == nullptr) {
        return {};
    }
    std::string result(error);
    sao_plugins_luahost_free_string(error);
    return result;
}

int32_t teardown_host_bridge(lua_host_handle_t host) {
#if defined(SAO_HAS_LUA)
    lua_State* state = sao_plugins_luahost_state(host);
    return state == nullptr ? SAO_ERR_HANDLE_INVALID : detail::teardown_ctx_bridge(state);
#else
    (void)host;
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

void finish_failed_load(lua_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                        std::string error) {
    std::lock_guard lock(g_adapter_mutex);
    if (owner != g_adapter_owner)
        return;
    owner->plugins.erase(plugin);
    owner->last_errors[plugin] = std::move(error);
}

void cleanup_failed_load(lua_loader_adapter_owner_s* owner, loader_plugin_handle_t plugin,
                         lua_host_handle_t host, lua_plugin_handle_t lua_plugin,
                         std::string error) noexcept {
    try {
        if (lua_plugin != nullptr && sao_plugins_luahost_unload_script(lua_plugin) == SAO_OK) {
            lua_plugin = nullptr;
        }
        if (lua_plugin == nullptr && host != nullptr) {
            const int32_t bridge_status = teardown_host_bridge(host);
            if (bridge_status == SAO_OK && sao_plugins_luahost_destroy(host) == SAO_OK) {
                host = nullptr;
            }
        }
        if (host == nullptr) {
            finish_failed_load(owner, plugin, std::move(error));
            return;
        }

        std::lock_guard lock(g_adapter_mutex);
        if (owner != g_adapter_owner)
            return;
        const auto found = owner->plugins.find(plugin);
        if (found == owner->plugins.end())
            return;
        found->second.host = host;
        found->second.plugin = lua_plugin;
        found->second.lifecycle = lua_plugin == nullptr
                                      ? adapter_plugin_record::state::host_destroy_pending
                                      : adapter_plugin_record::state::load_cleanup_pending;
        owner->last_errors[plugin] = std::move(error);
    } catch (...) {
    }
}

int32_t create_plugin_runtime(lua_loader_adapter_owner_s* owner,
                              const sao::plugins::loader::plugin_manifest& manifest,
                              sao::plugins::loader::plugin_context_t* context,
                              lua_host_handle_t& host, lua_plugin_handle_t& plugin,
                              std::string& error) {
#if !defined(SAO_HAS_LUA)
    (void)owner;
    (void)manifest;
    (void)context;
    (void)host;
    (void)plugin;
    (void)error;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    std::wstring plugin_dir;
    if (!utf8_to_wide(manifest.source_path, plugin_dir)) {
        error = "plugin source_path is not valid UTF-8";
        return SAO_ERR_INVALID_ARGUMENT;
    }

    lua_host_config host_config = owner->config;
    host_config.install_stdlib = false;
    int32_t status = sao_plugins_luahost_create(&host_config, &host);
    if (status != SAO_OK) {
        error = "Lua host creation failed";
        return status;
    }
    lua_State* state = sao_plugins_luahost_state(host);
    if (state == nullptr) {
        error = "Lua host state is unavailable";
        return SAO_ERR_NOT_INITIALIZED;
    }

    const bool unsafe = has_permission(manifest, "unsafe");
    const bool allow_fs = unsafe || has_permission(manifest, "fs");
    const bool allow_process = unsafe || has_permission(manifest, "process");
    const bool allow_require = unsafe || has_permission(manifest, "require");
    uint32_t permissions = detail::permission_none;
    const auto add_permission = [&permissions](bool allowed, detail::lua_permission value) {
        if (allowed)
            permissions |= static_cast<uint32_t>(value);
    };
    add_permission(allow_fs, detail::permission_fs);
    add_permission(unsafe || has_permission(manifest, "net"), detail::permission_net);
    add_permission(allow_process, detail::permission_process);
    add_permission(unsafe || has_permission(manifest, "hotkey"), detail::permission_hotkey);
    add_permission(unsafe || has_permission(manifest, "memory_access"),
                   detail::permission_memory_access);
    add_permission(unsafe || has_permission(manifest, "input_control"),
                   detail::permission_input_control);
    add_permission(unsafe || has_permission(manifest, "engine_access"),
                   detail::permission_engine_access);
    add_permission(unsafe, detail::permission_unsafe);
    status = detail::set_state_permissions(state, permissions);
    if (status != SAO_OK) {
        error = "Lua state permission setup failed";
        return status;
    }

    lua_stdlib_config stdlib{};
    stdlib.io = allow_fs;
    stdlib.os = allow_fs || allow_process;
    stdlib.package_ = allow_require;
    stdlib.debug_ = unsafe;
    status = sao_plugins_luahost_install_stdlib(state, &stdlib);
    if (status != SAO_OK) {
        error = "Lua standard library installation failed";
        return status;
    }
    status = sao_plugins_luahost_install_sao_stdlib(state);
    if (status != SAO_OK) {
        error = "Lua SAO library installation failed";
        return status;
    }

    lua_sandbox_config sandbox{};
    sandbox.allow_fs = allow_fs;
    sandbox.allow_net = unsafe || has_permission(manifest, "net");
    sandbox.allow_process = allow_process;
    sandbox.allow_require = allow_require;
    sandbox.max_instructions_per_run = owner->config.max_instructions_per_run;
    sandbox.max_memory_bytes = owner->config.max_memory_bytes;
    status = sao_plugins_luahost_sandbox_arm(state, &sandbox);
    if (status != SAO_OK) {
        error = "Lua sandbox setup failed";
        return status;
    }

    status = sao_plugins_luahost_load_script(state, plugin_dir.c_str(), manifest.entry.c_str(),
                                             manifest.plugin_id.c_str(), context, &plugin);
    if (status != SAO_OK) {
        error = take_state_error(host);
    }
    return status;
#endif
}

class plugin_call_lease {
  public:
    plugin_call_lease() = default;
    ~plugin_call_lease() {
        reset();
    }

    plugin_call_lease(const plugin_call_lease&) = delete;
    plugin_call_lease& operator=(const plugin_call_lease&) = delete;

    void assign(lua_loader_adapter_owner_s* owner, loader_plugin_handle_t loader_plugin,
                lua_host_handle_t host, lua_plugin_handle_t plugin) {
        owner_ = owner;
        loader_plugin_ = loader_plugin;
        host_ = host;
        plugin_ = plugin;
    }

    void reset() {
        if (owner_ == nullptr)
            return;
        std::lock_guard lock(g_adapter_mutex);
        const auto found = owner_->plugins.find(loader_plugin_);
        if (found != owner_->plugins.end() && found->second.host == host_ &&
            found->second.plugin == plugin_ && found->second.active_calls > 0) {
            --found->second.active_calls;
        }
        owner_ = nullptr;
        loader_plugin_ = nullptr;
        host_ = nullptr;
        plugin_ = nullptr;
    }

    lua_host_handle_t host() const {
        return host_;
    }
    lua_plugin_handle_t plugin() const {
        return plugin_;
    }

  private:
    lua_loader_adapter_owner_s* owner_ = nullptr;
    loader_plugin_handle_t loader_plugin_ = nullptr;
    lua_host_handle_t host_ = nullptr;
    lua_plugin_handle_t plugin_ = nullptr;
};

int32_t acquire_plugin(void* user_data, loader_plugin_handle_t plugin, plugin_call_lease& lease) {
    std::lock_guard lock(g_adapter_mutex);
    auto* owner = active_owner(user_data);
    if (owner == nullptr)
        return SAO_ERR_NOT_INITIALIZED;
    const auto found = owner->plugins.find(plugin);
    if (found == owner->plugins.end() || found->second.host == nullptr ||
        found->second.plugin == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    if (found->second.lifecycle != adapter_plugin_record::state::ready) {
        return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
    }
    ++found->second.active_calls;
    lease.assign(owner, plugin, found->second.host, found->second.plugin);
    return SAO_OK;
}

template <typename Callback>
int32_t with_plugin(loader_plugin_handle_t plugin, void* user_data, Callback&& callback) {
    plugin_call_lease lease;
    int32_t status = acquire_plugin(user_data, plugin, lease);
    if (status != SAO_OK)
        return status;
    lua_State* state = nullptr;
#if defined(SAO_HAS_LUA)
    state = sao_plugins_luahost_state(lease.host());
    const auto state_mutex = detail::bridge_mutex(state);
    if (!state_mutex)
        return SAO_ERR_NOT_INITIALIZED;
    std::lock_guard state_lock(*state_mutex);
#endif
    status = callback(lease.plugin(), state);
    if (status != SAO_OK) {
        std::string error = take_state_error(lease.host());
        if (!error.empty()) {
            retain_error(static_cast<lua_loader_adapter_owner_s*>(user_data), plugin,
                         std::move(error));
        }
    }
    return status;
}

int32_t SAO_PLUGINS_CALL adapter_load(loader_plugin_handle_t plugin,
                                      const sao::plugins::loader::plugin_manifest* manifest,
                                      void* user_data) {
    lua_loader_adapter_owner_s* owner = nullptr;
    lua_host_handle_t host = nullptr;
    lua_plugin_handle_t lua_plugin = nullptr;
    bool reserved = false;
    try {
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            if (plugin == nullptr || manifest == nullptr || manifest->source_path.empty() ||
                manifest->entry.empty()) {
                return SAO_ERR_INVALID_ARGUMENT;
            }
            if (owner->plugins.contains(plugin)) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
            }
            owner->last_errors.erase(plugin);
            owner->plugins.emplace(plugin, adapter_plugin_record{});
            reserved = true;
        }

        sao::plugins::loader::plugin_context_t* context = nullptr;
        int32_t status = sao::plugins::loader::sao_plugins_lifecycle_get_context(plugin, &context);
        std::string error;
        if (status == SAO_OK) {
            status = create_plugin_runtime(owner, *manifest, context, host, lua_plugin, error);
        }
        if (status != SAO_OK) {
            cleanup_failed_load(owner, plugin, host, lua_plugin,
                                error.empty() ? "Lua plugin load failed" : std::move(error));
            return status;
        }

        {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() ||
                found->second.lifecycle != adapter_plugin_record::state::loading) {
                status = SAO_ERR_HANDLE_INVALID;
            } else {
                found->second.host = host;
                found->second.plugin = lua_plugin;
                found->second.lifecycle = adapter_plugin_record::state::ready;
                host = nullptr;
                lua_plugin = nullptr;
                reserved = false;
            }
        }
        if (status != SAO_OK) {
            cleanup_failed_load(owner, plugin, host, lua_plugin, "Lua plugin load was cancelled");
        }
        return status;
    } catch (...) {
        if (reserved && owner != nullptr) {
            cleanup_failed_load(owner, plugin, host, lua_plugin, "Lua plugin load failed");
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_load(loader_plugin_handle_t plugin, void* user_data) {
    try {
        return with_plugin(plugin, user_data, [](lua_plugin_handle_t value, lua_State*) {
            return sao_plugins_luahost_call_on_load(value);
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_enable(loader_plugin_handle_t plugin, void* user_data) {
    try {
        return with_plugin(plugin, user_data, [](lua_plugin_handle_t value, lua_State* state) {
#if !defined(SAO_HAS_LUA)
            (void)state;
            return sao_plugins_luahost_call_on_enable(value);
#else
            const std::size_t checkpoint = detail::ctx_menu_checkpoint_locked(state);
            const int32_t status = sao_plugins_luahost_call_on_enable(value);
            if (status == SAO_OK) {
                return detail::commit_ctx_enable_menus_locked(state, checkpoint);
            }
            const int32_t rollback_status = detail::rollback_ctx_menus_locked(state, checkpoint);
            return rollback_status == SAO_OK ? status : rollback_status;
#endif
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_disable(loader_plugin_handle_t plugin, void* user_data) {
    try {
        return with_plugin(plugin, user_data, [](lua_plugin_handle_t value, lua_State* state) {
#if !defined(SAO_HAS_LUA)
            (void)state;
            return sao_plugins_luahost_call_on_disable(value);
#else
            const int32_t status = sao_plugins_luahost_call_on_disable(value);
            return status == SAO_OK ? detail::remove_ctx_enable_menus_locked(state) : status;
#endif
        });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_on_unload(loader_plugin_handle_t plugin, bool* allow_unload,
                                           void* user_data) {
    if (allow_unload != nullptr)
        *allow_unload = true;
    try {
        plugin_call_lease lease;
        lua_loader_adapter_owner_s* owner = nullptr;
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || found->second.host == nullptr) {
                return SAO_ERR_HANDLE_INVALID;
            }
            if (found->second.lifecycle == adapter_plugin_record::state::load_cleanup_pending ||
                found->second.lifecycle == adapter_plugin_record::state::host_destroy_pending) {
                if (found->second.active_calls != 0 && allow_unload != nullptr)
                    *allow_unload = false;
                return SAO_OK;
            }
            if (found->second.plugin == nullptr)
                return SAO_ERR_HANDLE_INVALID;
            if (found->second.lifecycle != adapter_plugin_record::state::ready) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            if (found->second.active_calls != 0) {
                if (allow_unload != nullptr)
                    *allow_unload = false;
                return SAO_OK;
            }
            found->second.lifecycle = adapter_plugin_record::state::unloading;
            ++found->second.active_calls;
            lease.assign(owner, plugin, found->second.host, found->second.plugin);
        }
#if defined(SAO_HAS_LUA)
        lua_State* state = sao_plugins_luahost_state(lease.host());
        const auto state_mutex = detail::bridge_mutex(state);
        if (!state_mutex) {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found != owner->plugins.end()) {
                found->second.lifecycle = adapter_plugin_record::state::ready;
            }
            return SAO_ERR_NOT_INITIALIZED;
        }
        std::lock_guard state_lock(*state_mutex);
#endif
        const int32_t status = sao_plugins_luahost_call_on_unload(lease.plugin(), allow_unload);
        if (status != SAO_OK) {
            std::string error = take_state_error(lease.host());
            if (!error.empty())
                retain_error(owner, plugin, std::move(error));
        }
        if (status != SAO_OK || (allow_unload != nullptr && !*allow_unload)) {
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found != owner->plugins.end() &&
                found->second.lifecycle == adapter_plugin_record::state::unloading) {
                found->second.lifecycle = adapter_plugin_record::state::ready;
            }
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t SAO_PLUGINS_CALL adapter_unload(loader_plugin_handle_t plugin, void* user_data) {
    try {
        lua_loader_adapter_owner_s* owner = nullptr;
        lua_host_handle_t host = nullptr;
        lua_plugin_handle_t lua_plugin = nullptr;
        bool destroy_host_only = false;
        bool load_cleanup_pending = false;
        {
            std::lock_guard lock(g_adapter_mutex);
            owner = active_owner(user_data);
            if (owner == nullptr)
                return SAO_ERR_NOT_INITIALIZED;
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end() || found->second.host == nullptr) {
                return SAO_ERR_HANDLE_INVALID;
            }
            if (found->second.lifecycle == adapter_plugin_record::state::host_destroy_pending) {
                if (found->second.plugin != nullptr || found->second.active_calls != 0) {
                    return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
                }
                destroy_host_only = true;
            } else if (found->second.lifecycle ==
                       adapter_plugin_record::state::load_cleanup_pending) {
                if (found->second.plugin == nullptr || found->second.active_calls != 0) {
                    return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
                }
                load_cleanup_pending = true;
            } else if (found->second.plugin == nullptr ||
                       (found->second.lifecycle != adapter_plugin_record::state::ready &&
                        found->second.lifecycle != adapter_plugin_record::state::unloading) ||
                       found->second.active_calls != 0) {
                return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
            }
            found->second.lifecycle = adapter_plugin_record::state::unloading;
            host = found->second.host;
            lua_plugin = found->second.plugin;
        }

        if (!destroy_host_only) {
            const int32_t script_status = sao_plugins_luahost_unload_script(lua_plugin);
            if (script_status != SAO_OK) {
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end()) {
                    found->second.lifecycle =
                        load_cleanup_pending ? adapter_plugin_record::state::load_cleanup_pending
                                             : adapter_plugin_record::state::ready;
                    owner->last_errors[plugin] = "Lua plugin unload failed";
                }
                return script_status;
            }
            std::lock_guard lock(g_adapter_mutex);
            const auto found = owner->plugins.find(plugin);
            if (found == owner->plugins.end())
                return SAO_ERR_HANDLE_INVALID;
            found->second.plugin = nullptr;
            found->second.lifecycle = adapter_plugin_record::state::host_destroy_pending;
        }

        if (destroy_host_only) {
            const int32_t bridge_status = teardown_host_bridge(host);
            if (bridge_status != SAO_OK) {
                std::lock_guard lock(g_adapter_mutex);
                const auto found = owner->plugins.find(plugin);
                if (found != owner->plugins.end()) {
                    found->second.lifecycle = adapter_plugin_record::state::host_destroy_pending;
                    owner->last_errors[plugin] = "Lua bridge teardown failed";
                }
                return bridge_status;
            }
        }

        const int32_t status = sao_plugins_luahost_destroy(host);
        std::lock_guard lock(g_adapter_mutex);
        const auto found = owner->plugins.find(plugin);
        if (status == SAO_OK) {
            if (found != owner->plugins.end())
                owner->plugins.erase(found);
        } else {
            owner->last_errors[plugin] = "Lua host destroy failed";
            if (found != owner->plugins.end()) {
                found->second.lifecycle = adapter_plugin_record::state::host_destroy_pending;
            }
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

sao::plugins::loader::host_adapter_vtable adapter_vtable(lua_loader_adapter_owner_s* owner) {
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

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_luahost_register_loader_adapter(
    const lua_host_config* config, lua_loader_adapter_owner_t* out_owner) {
    if (out_owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_owner = nullptr;
#if !defined(SAO_HAS_LUA)
    (void)config;
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        std::lock_guard lock(g_adapter_mutex);
        if (g_adapter_owner != nullptr) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_ALREADY_EXISTS;
        }
        auto owner = std::make_unique<lua_loader_adapter_owner_s>();
        if (config != nullptr)
            owner->config = *config;
        const auto table = adapter_vtable(owner.get());
        const int32_t status = sao::plugins::loader::sao_plugins_lifecycle_register_host_adapter(
            sao::plugins::loader::engine_kind::lua, &table);
        if (status != SAO_OK)
            return status;
        owner->active = true;
        g_adapter_owner = owner.get();
        *out_owner = owner.release();
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_unregister_loader_adapter(lua_loader_adapter_owner_t owner) {
    if (owner == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if !defined(SAO_HAS_LUA)
    return SAO_ERR_NOT_IMPLEMENTED;
#else
    try {
        std::unique_lock lock(g_adapter_mutex);
        if (owner != g_adapter_owner || !owner->active) {
            return SAO_ERR_HANDLE_INVALID;
        }
        if (!owner->plugins.empty()) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        lock.unlock();
        const int32_t status = sao::plugins::loader::sao_plugins_lifecycle_unregister_host_adapter(
            sao::plugins::loader::engine_kind::lua);
        if (status != SAO_OK)
            return status;
        lock.lock();
        owner->active = false;
        g_adapter_owner = nullptr;
        lock.unlock();
        delete owner;
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
#endif
}

extern "C" SAO_PLUGINS_API size_t SAO_PLUGINS_CALL
sao_plugins_luahost_loader_adapter_plugin_count(lua_loader_adapter_owner_t owner) {
    try {
        std::lock_guard lock(g_adapter_mutex);
        return owner != nullptr && owner == g_adapter_owner && owner->active ? owner->plugins.size()
                                                                             : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_loader_adapter_get_last_error(lua_loader_adapter_owner_t owner,
                                                  void* loader_plugin_handle, char** out_utf8) {
    if (out_utf8 == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    *out_utf8 = nullptr;
    try {
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        std::string error;
        {
            std::lock_guard lock(g_adapter_mutex);
            if (owner == nullptr || owner != g_adapter_owner || !owner->active) {
                return SAO_ERR_HANDLE_INVALID;
            }
            const auto found = owner->last_errors.find(plugin);
            if (found != owner->last_errors.end())
                error = found->second;
        }
        return copy_string(error, out_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL sao_plugins_luahost_loader_adapter_call_hook(
    lua_loader_adapter_owner_t owner, void* loader_plugin_handle, const char* hook_name,
    const char* args_json_utf8, char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr)
        *out_result_json_utf8 = nullptr;
    if (owner == nullptr || loader_plugin_handle == nullptr || hook_name == nullptr ||
        hook_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        auto* plugin = static_cast<sao::plugins::loader::plugin_handle_s*>(loader_plugin_handle);
        return with_plugin(plugin, owner,
                           [hook_name, args_json_utf8,
                            out_result_json_utf8](lua_plugin_handle_t value, lua_State*) {
                               return sao_plugins_luahost_call_hook(
                                   value, hook_name, args_json_utf8, out_result_json_utf8);
                           });
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace sao::plugins::lua_host
