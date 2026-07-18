#include "sao/plugins/lua_host/lua_call.h"

#include "sao/plugins/lua_host/lua_module_bridge.h"
#include "sao/plugins/loader/loader_status.h"

#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "lua_json_internal.h"
#include "lua_bridge_internal.h"
#include "lua_sandbox_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)

namespace fs = std::filesystem;

struct lua_plugin_s {
    lua_State* state = nullptr;
    void* context = nullptr;
    int context_ref = LUA_NOREF;
    std::shared_ptr<std::recursive_mutex> state_mutex;
    std::unordered_map<std::string, int> lifecycle_refs;
};

namespace {

struct plugin_control final {
    explicit plugin_control(lua_plugin_s* value) : plugin(value) {}
    lua_plugin_s* plugin = nullptr;
    std::recursive_mutex mutex;
    bool closing = false;
    size_t active_operations = 0;
};

std::mutex g_plugins_mutex;
std::unordered_map<lua_plugin_s*, std::shared_ptr<plugin_control>> g_plugins;

class plugin_operation final {
public:
    plugin_operation() = default;
    ~plugin_operation() {
        if (control_ != nullptr && control_->active_operations > 0) {
            --control_->active_operations;
        }
    }
    plugin_operation(const plugin_operation&) = delete;
    plugin_operation& operator=(const plugin_operation&) = delete;

    lua_plugin_s* plugin() const noexcept {
        return control_ == nullptr ? nullptr : control_->plugin;
    }

    std::shared_ptr<plugin_control> control_;
    std::unique_lock<std::recursive_mutex> lock_;
};

int32_t acquire_plugin(lua_plugin_handle_t handle,
                       plugin_operation& operation) {
    if (handle == nullptr) return SAO_ERR_HANDLE_INVALID;
    std::shared_ptr<plugin_control> control;
    {
        std::lock_guard map_lock(g_plugins_mutex);
        const auto found = g_plugins.find(handle);
        if (found == g_plugins.end()) return SAO_ERR_HANDLE_INVALID;
        control = found->second;
    }
    std::unique_lock lock(control->mutex);
    if (control->closing || control->plugin != handle) {
        return SAO_ERR_HANDLE_INVALID;
    }
    ++control->active_operations;
    operation.control_ = std::move(control);
    operation.lock_ = std::move(lock);
    return SAO_OK;
}

constexpr const char* kLifecycleHooks[] = {
    "on_load", "on_enable", "on_disable", "on_unload"};

std::string path_to_utf8(const fs::path& path) {
    const auto& value = path.native();
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            needed, nullptr, nullptr) != needed) {
        return {};
    }
    return result;
}

bool valid_relative_entry(const fs::path& entry) {
    if (entry.empty() || entry.is_absolute() || entry.has_root_path()) {
        return false;
    }
    for (const auto& component : entry) {
        if (component == L"..") return false;
    }
    return true;
}

bool path_is_within(const fs::path& root, const fs::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() ||
            _wcsicmp(root_part->c_str(), candidate_part->c_str()) != 0) {
            return false;
        }
    }
    return true;
}

int32_t read_script(const wchar_t* plugin_dir, const char* entry_relative,
                    std::string& source, std::string& chunk_name) {
    if (plugin_dir == nullptr || plugin_dir[0] == L'\0' ||
        entry_relative == nullptr || entry_relative[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        std::error_code filesystem_error;
        const fs::path root = fs::weakly_canonical(
            fs::path(plugin_dir), filesystem_error);
        if (filesystem_error || !fs::is_directory(root, filesystem_error)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        const fs::path entry = fs::u8path(entry_relative);
        if (!valid_relative_entry(entry)) return SAO_ERR_INVALID_ARGUMENT;
        const fs::path absolute = fs::weakly_canonical(
            root / entry, filesystem_error);
        if (filesystem_error || !path_is_within(root, absolute) ||
            !fs::is_regular_file(absolute, filesystem_error)) {
            return SAO_ERR_HANDLE_INVALID;
        }
        std::ifstream input(absolute, std::ios::binary);
        if (!input) return SAO_ERR_HANDLE_INVALID;
        source.assign(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
        if (!input.eof() && input.fail()) return SAO_ERR_OS_CALL_FAILED;
        chunk_name = "@" + path_to_utf8(absolute);
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t configure_package_path(lua_State* state, const wchar_t* plugin_dir) {
    try {
        std::error_code filesystem_error;
        const fs::path root =
            fs::weakly_canonical(fs::path(plugin_dir), filesystem_error);
        if (filesystem_error || root.empty()) return SAO_ERR_HANDLE_INVALID;
        const std::string root_utf8 = path_to_utf8(root);
        const std::string libs_utf8 = path_to_utf8(root / L"libs");
        const std::string vendor_utf8 = path_to_utf8(root / L"vendor");
        if (root_utf8.empty() || root_utf8.find_first_of(";?") !=
                                     std::string::npos) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        const std::string path =
            root_utf8 + "/?.lua;" + root_utf8 + "/?/init.lua;" +
            libs_utf8 + "/?.lua;" + libs_utf8 + "/?/init.lua;" +
            vendor_utf8 + "/?.lua;" + vendor_utf8 + "/?/init.lua";
        return detail::sandbox_set_controlled_package_path_locked(
            state, path.data(), path.size(), root_utf8.data(),
            root_utf8.size());
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int lifecycle_ref(lua_plugin_handle_t plugin, const char* name) {
    const auto found = plugin->lifecycle_refs.find(name);
    return found == plugin->lifecycle_refs.end() ? LUA_NOREF : found->second;
}

int32_t copy_json_result(lua_State* state, int index, char** output) {
    if (output == nullptr) return SAO_OK;
    detail::json value;
    std::string error;
    if (!detail::stack_to_json(state, index, value, error)) {
        detail::set_state_error_locked(state, std::move(error));
        return SAO_ERR_INVALID_ARGUMENT;
    }
    const std::string serialized = value.dump();
    auto* copy = static_cast<char*>(std::malloc(serialized.size() + 1));
    if (copy == nullptr) {
        detail::set_state_error_locked(state,
                                       "failed to allocate Lua JSON result");
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (!serialized.empty()) {
        std::memcpy(copy, serialized.data(), serialized.size());
    }
    copy[serialized.size()] = '\0';
    *output = copy;
    return SAO_OK;
}

int32_t call_ref(lua_State* state, int registry_ref,
                 const char* args_json_utf8, char** output) {
    if (output != nullptr) *output = nullptr;
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    if (registry_ref == LUA_NOREF || registry_ref == LUA_REFNIL) {
        return SAO_ERR_HANDLE_INVALID;
    }

    const int base = lua_gettop(state);
    detail::clear_state_error_locked(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, registry_ref);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, base);
        return SAO_ERR_HANDLE_INVALID;
    }
    int argument_count = 0;
    std::string argument_error;
    if (!detail::push_json_arguments(state, args_json_utf8, argument_count,
                                     argument_error)) {
        if (lua_gettop(state) > base &&
            lua_type(state, -1) == LUA_TSTRING) {
            detail::capture_state_error_locked(state, -1);
        } else {
            detail::set_state_error_locked(state, std::move(argument_error));
        }
        lua_settop(state, base);
        return SAO_ERR_INVALID_ARGUMENT;
    }
    if (lua_pcall(state, argument_count, 1, 0) != LUA_OK) {
        detail::capture_state_error_locked(state, -1);
        lua_settop(state, base);
        return SAO_ERR_OS_CALL_FAILED;
    }
    const int32_t status = copy_json_result(state, -1, output);
    lua_settop(state, base);
    return status;
}

int32_t call_lifecycle(lua_plugin_handle_t plugin, const char* hook,
                       bool* allow_unload) {
    if (allow_unload != nullptr) *allow_unload = true;
    plugin_operation plugin_lease;
    int32_t status = acquire_plugin(plugin, plugin_lease);
    if (status != SAO_OK) return status;
    lua_plugin_s* live_plugin = plugin_lease.plugin();
    if (live_plugin == nullptr || live_plugin->state == nullptr) {
        return SAO_ERR_HANDLE_INVALID;
    }
    detail::state_operation state_lease;
    status = detail::acquire_state_operation(live_plugin->state,
                                             state_lease);
    if (status != SAO_OK) return status;
    const int reference = lifecycle_ref(live_plugin, hook);
    if (reference == LUA_NOREF || reference == LUA_REFNIL) return SAO_OK;

    lua_State* state = live_plugin->state;
    const int base = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    int argument_count = 0;
    if (live_plugin->context_ref != LUA_NOREF &&
        live_plugin->context_ref != LUA_REFNIL) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, live_plugin->context_ref);
        argument_count = 1;
    }
    if (lua_pcall(state, argument_count, 1, 0) != LUA_OK) {
        detail::capture_state_error_locked(state, -1);
        lua_settop(state, base);
        return SAO_ERR_OS_CALL_FAILED;
    }
    if (allow_unload != nullptr && lua_isboolean(state, -1)) {
        *allow_unload = lua_toboolean(state, -1) != 0;
    }
    lua_settop(state, base);
    return SAO_OK;
}

int capture_plugin_refs_body(lua_State* state) {
    auto* plugin = static_cast<lua_plugin_s*>(lua_touserdata(state, 1));
    if (plugin == nullptr) return luaL_error(state, "invalid Lua plugin");
    try {
        if (plugin->context != nullptr) {
            lua_getglobal(state, "ctx");
            plugin->context_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        }
        for (const char* hook : kLifecycleHooks) {
            lua_getglobal(state, hook);
            if (lua_isfunction(state, -1)) {
                const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
                try {
                    plugin->lifecycle_refs.emplace(hook, reference);
                } catch (...) {
                    luaL_unref(state, LUA_REGISTRYINDEX, reference);
                    throw;
                }
            } else {
                lua_pop(state, 1);
            }
        }
        return 0;
    } catch (...) {
        return luaL_error(state, "failed to cache Lua plugin hooks");
    }
}

struct hook_lookup_data final {
    const char* name = nullptr;
    int reference = LUA_NOREF;
};

int lookup_hook_body(lua_State* state) {
    auto* data =
        static_cast<hook_lookup_data*>(lua_touserdata(state, 1));
    lua_getglobal(state, data->name);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    data->reference = luaL_ref(state, LUA_REGISTRYINDEX);
    return 0;
}

void release_plugin_refs_locked(lua_plugin_s& plugin) {
    for (const auto& [name, reference] : plugin.lifecycle_refs) {
        (void)name;
        luaL_unref(plugin.state, LUA_REGISTRYINDEX, reference);
    }
    plugin.lifecycle_refs.clear();
    if (plugin.context_ref != LUA_NOREF &&
        plugin.context_ref != LUA_REFNIL) {
        luaL_unref(plugin.state, LUA_REGISTRYINDEX, plugin.context_ref);
        plugin.context_ref = LUA_NOREF;
    }
}

} // namespace

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_load_script(lua_State* state,
                                const wchar_t* plugin_dir,
                                const char* entry_relative,
                                const char* plugin_id_utf8,
                                void* context,
                                lua_plugin_handle_t* out_plugin) {
    if (out_plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_plugin = nullptr;
    if (state == nullptr || plugin_dir == nullptr || entry_relative == nullptr ||
        plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    bool bridge_registered = false;
    try {
        std::string source;
        std::string chunk_name;
        int32_t status =
            read_script(plugin_dir, entry_relative, source, chunk_name);
        if (status != SAO_OK) return status;

        detail::state_operation state_lease;
        status = detail::acquire_state_operation(state, state_lease);
        if (status != SAO_OK) return status;
        const int base = lua_gettop(state);
        status = configure_package_path(state, plugin_dir);
        if (status != SAO_OK) {
            lua_settop(state, base);
            return status;
        }
        if (context != nullptr) {
            status = sao_plugins_luahost_register_ctx(state, context);
            if (status != SAO_OK) {
                lua_settop(state, base);
                return status;
            }
            bridge_registered = true;
        }
        if (luaL_loadbufferx(state, source.data(), source.size(),
                             chunk_name.c_str(), "t") != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            if (context != nullptr) {
                (void)detail::teardown_ctx_bridge_locked(state);
            }
            return SAO_ERR_INVALID_ARGUMENT;
        }
        if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            if (context != nullptr) {
                (void)detail::teardown_ctx_bridge_locked(state);
            }
            return SAO_ERR_OS_CALL_FAILED;
        }

        auto plugin = std::make_unique<lua_plugin_s>();
        plugin->state = state;
        plugin->context = context;
        plugin->state_mutex = detail::state_mutex(state);
        if (!plugin->state_mutex) {
            if (context != nullptr) {
                (void)detail::teardown_ctx_bridge_locked(state);
            }
            lua_settop(state, base);
            return SAO_ERR_HANDLE_INVALID;
        }
        if (detail::protected_trampoline(state, capture_plugin_refs_body,
                                         plugin.get(), 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            release_plugin_refs_locked(*plugin);
            if (context != nullptr) {
                (void)detail::teardown_ctx_bridge_locked(state);
            }
            return SAO_ERR_OS_CALL_FAILED;
        }
        lua_settop(state, base);
        std::shared_ptr<plugin_control> control;
        try {
            control = std::make_shared<plugin_control>(plugin.get());
            {
                std::lock_guard map_lock(g_plugins_mutex);
                if (!g_plugins.emplace(plugin.get(), control).second) {
                    release_plugin_refs_locked(*plugin);
                    if (context != nullptr) {
                        (void)detail::teardown_ctx_bridge_locked(state);
                    }
                    return SAO_ERR_OS_CALL_FAILED;
                }
            }
        } catch (...) {
            release_plugin_refs_locked(*plugin);
            if (context != nullptr) {
                (void)detail::teardown_ctx_bridge_locked(state);
            }
            return SAO_ERR_OS_CALL_FAILED;
        }
        status = detail::add_live_plugin_locked(state);
        if (status != SAO_OK) {
            {
                std::lock_guard map_lock(g_plugins_mutex);
                g_plugins.erase(plugin.get());
            }
            release_plugin_refs_locked(*plugin);
            if (context != nullptr) {
                (void)detail::teardown_ctx_bridge_locked(state);
            }
            return status;
        }
        *out_plugin = plugin.release();
        return SAO_OK;
    } catch (...) {
        if (bridge_registered) (void)detail::teardown_ctx_bridge(state);
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_load(lua_plugin_handle_t plugin) {
    try {
        return call_lifecycle(plugin, "on_load", nullptr);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_enable(lua_plugin_handle_t plugin) {
    try {
        return call_lifecycle(plugin, "on_enable", nullptr);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_disable(lua_plugin_handle_t plugin) {
    try {
        return call_lifecycle(plugin, "on_disable", nullptr);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_unload(lua_plugin_handle_t plugin,
                                   bool* out_allow_unload) {
    if (out_allow_unload != nullptr) *out_allow_unload = true;
    try {
        return call_lifecycle(plugin, "on_unload", out_allow_unload);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_hook(lua_plugin_handle_t plugin,
                              const char* hook_name,
                              const char* args_json_utf8,
                              char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    if (plugin == nullptr || hook_name == nullptr || hook_name[0] == '\0') {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    try {
        plugin_operation plugin_lease;
        int32_t status = acquire_plugin(plugin, plugin_lease);
        if (status != SAO_OK) return status;
        lua_plugin_s* live_plugin = plugin_lease.plugin();
        detail::state_operation state_lease;
        status = detail::acquire_state_operation(live_plugin->state,
                                                 state_lease);
        if (status != SAO_OK) return status;
        int reference = lifecycle_ref(live_plugin, hook_name);
        bool temporary_reference = false;
        if (reference == LUA_NOREF) {
            hook_lookup_data lookup{hook_name, LUA_NOREF};
            if (detail::protected_trampoline(live_plugin->state,
                                             lookup_hook_body, &lookup,
                                             0) != LUA_OK) {
                detail::capture_state_error_locked(live_plugin->state, -1);
                lua_pop(live_plugin->state, 1);
                return SAO_ERR_OS_CALL_FAILED;
            }
            reference = lookup.reference;
            if (reference == LUA_NOREF) return SAO_OK;
            temporary_reference = true;
        }
        status = call_ref(live_plugin->state, reference, args_json_utf8,
                          out_result_json_utf8);
        if (temporary_reference) {
            luaL_unref(live_plugin->state, LUA_REGISTRYINDEX, reference);
        }
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_has_hook(lua_plugin_handle_t plugin,
                             const char* hook_name) {
    if (plugin == nullptr || hook_name == nullptr || hook_name[0] == '\0') {
        return false;
    }
    try {
        plugin_operation plugin_lease;
        if (acquire_plugin(plugin, plugin_lease) != SAO_OK) return false;
        lua_plugin_s* live_plugin = plugin_lease.plugin();
        detail::state_operation state_lease;
        if (detail::acquire_state_operation(live_plugin->state,
                                            state_lease) != SAO_OK) {
            return false;
        }
        if (lifecycle_ref(live_plugin, hook_name) != LUA_NOREF) return true;
        hook_lookup_data lookup{hook_name, LUA_NOREF};
        if (detail::protected_trampoline(live_plugin->state, lookup_hook_body,
                                         &lookup, 0) != LUA_OK) {
            detail::capture_state_error_locked(live_plugin->state, -1);
            lua_pop(live_plugin->state, 1);
            return false;
        }
        if (lookup.reference == LUA_NOREF) return false;
        luaL_unref(live_plugin->state, LUA_REGISTRYINDEX, lookup.reference);
        return true;
    } catch (...) {
        return false;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_unload_script(lua_plugin_handle_t plugin) {
    if (plugin == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        std::shared_ptr<plugin_control> control;
        {
            std::lock_guard map_lock(g_plugins_mutex);
            const auto found = g_plugins.find(plugin);
            if (found == g_plugins.end()) return SAO_ERR_HANDLE_INVALID;
            control = found->second;
        }
        std::unique_lock plugin_lock(control->mutex);
        if (control->closing) {
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        control->closing = true;
        if (control->active_operations != 0 || control->plugin != plugin) {
            control->closing = false;
            return sao::plugins::loader::SAO_PLUGINS_ERR_BUSY;
        }
        detail::state_operation state_lease;
        const int32_t state_status =
            detail::acquire_state_operation(plugin->state, state_lease);
        if (state_status != SAO_OK) {
            control->closing = false;
            return state_status;
        }
        int32_t status = SAO_OK;
        if (plugin->context != nullptr) {
            status = detail::teardown_ctx_bridge_locked(plugin->state);
        }
        release_plugin_refs_locked(*plugin);
        detail::remove_live_plugin_locked(plugin->state);
        {
            std::lock_guard map_lock(g_plugins_mutex);
            const auto found = g_plugins.find(plugin);
            if (found != g_plugins.end() && found->second == control) {
                g_plugins.erase(found);
            }
        }
        control->plugin = nullptr;
        plugin_lock.unlock();
        delete plugin;
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function_by_ref(lua_State* state,
                                         int registry_ref,
                                         const char* args_json_utf8,
                                         char** out_result_json_utf8) {
    if (out_result_json_utf8 != nullptr) *out_result_json_utf8 = nullptr;
    if (state == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    try {
        detail::state_operation state_lease;
        const int32_t status =
            detail::acquire_state_operation(state, state_lease);
        if (status != SAO_OK) return status;
        return call_ref(state, registry_ref, args_json_utf8,
                        out_result_json_utf8);
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

#else

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_load_script(lua_State*, const wchar_t*, const char*,
                                const char*, void*, lua_plugin_handle_t* output) {
    if (output == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *output = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_load(lua_plugin_handle_t) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_enable(lua_plugin_handle_t) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_disable(lua_plugin_handle_t) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_on_unload(lua_plugin_handle_t,
                                   bool* out_allow_unload) {
    if (out_allow_unload != nullptr) *out_allow_unload = true;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_hook(lua_plugin_handle_t, const char*, const char*,
                              char** output) {
    if (output != nullptr) *output = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_has_hook(lua_plugin_handle_t, const char*) {
    return false;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_unload_script(lua_plugin_handle_t) {
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_call_function_by_ref(lua_State*, int, const char*,
                                         char** output) {
    if (output != nullptr) *output = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

#endif

} // namespace sao::plugins::lua_host
