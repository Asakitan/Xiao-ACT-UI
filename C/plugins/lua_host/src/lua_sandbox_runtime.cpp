#include "sao/plugins/lua_host/lua_sandbox.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(SAO_HAS_LUA)
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}
#include "lua_sandbox_internal.h"
#include "lua_state_internal.h"
#endif

namespace sao::plugins::lua_host {

#if defined(SAO_HAS_LUA)
namespace {

struct sandbox_state final {
    lua_Alloc original_allocator = nullptr;
    void* original_allocator_data = nullptr;
    lua_Hook original_hook = nullptr;
    int original_hook_mask = 0;
    int original_hook_count = 0;
    uint64_t used_bytes = 0;
    uint64_t max_bytes = 0;
    int original_globals_ref = LUA_NOREF;
    int original_package_ref = LUA_NOREF;
    int original_package_path_ref = LUA_NOREF;
    int original_package_cpath_ref = LUA_NOREF;
    int original_package_searchers_ref = LUA_NOREF;
};

struct package_path_data final {
    const char* path = nullptr;
    size_t length = 0;
    int package_ref = LUA_NOREF;
    const char* root = nullptr;
    size_t root_length = 0;
};

struct restore_package_data final {
    int package_ref = LUA_NOREF;
    int path_ref = LUA_NOREF;
    int cpath_ref = LUA_NOREF;
    int searchers_ref = LUA_NOREF;
};

struct package_path_layer final {
    std::uint64_t id = 0;
    std::string path;
    std::string root;
};

struct package_path_layers final {
    int package_ref = LUA_NOREF;
    int path_ref = LUA_NOREF;
    int cpath_ref = LUA_NOREF;
    int searchers_ref = LUA_NOREF;
    std::vector<package_path_layer> layers;
};

void restore_package_ref(lua_State* state, int package_index, const char* field, int reference);

std::mutex g_sandboxes_mutex;
std::unordered_map<lua_State*, std::unique_ptr<sandbox_state>> g_sandboxes;
std::mutex g_package_layers_mutex;
std::unordered_map<lua_State*, package_path_layers> g_package_layers;
std::uint64_t g_next_package_layer_id = 1;

bool valid_module_name(const char* name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '.' || name[0] == '/' || name[0] == '\\') {
        return false;
    }
    bool previous_dot = false;
    for (const unsigned char value : std::string_view(name)) {
        if (value == '.') {
            if (previous_dot)
                return false;
            previous_dot = true;
            continue;
        }
        previous_dot = false;
        if (value == '/' || value == '\\' || value == ':' ||
            !(std::isalnum(value) || value == '_' || value == '-')) {
            return false;
        }
    }
    return !previous_dot;
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
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

bool controlled_file(const char* root_utf8, size_t root_length, const char* filename) {
    if (root_utf8 == nullptr || root_length == 0 || filename == nullptr) {
        return false;
    }
    try {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(
            std::filesystem::u8path(std::string(root_utf8, root_length)), error);
        if (error || root.empty())
            return false;
        const auto candidate =
            std::filesystem::weakly_canonical(std::filesystem::u8path(filename), error);
        return !error && path_is_within(root, candidate) &&
               std::filesystem::is_regular_file(candidate, error) && !error;
    } catch (...) {
        return false;
    }
}

int controlled_lua_searcher(lua_State* state) {
    const char* module_name = luaL_checkstring(state, 1);
    if (!valid_module_name(module_name)) {
        lua_pushliteral(state, "\n\tinvalid Lua module name");
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_pushliteral(state, "\n\tpackage table is unavailable");
        return 1;
    }
    lua_getfield(state, -1, "searchpath");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        lua_pushliteral(state, "\n\tpackage.searchpath is unavailable");
        return 1;
    }
    lua_pushstring(state, module_name);
    lua_getfield(state, -3, "path");
    lua_call(state, 2, 2);
    if (lua_isnil(state, -2)) {
        lua_remove(state, -2);
        return 1;
    }
    lua_pop(state, 1);
    const char* filename = lua_tostring(state, -1);
    size_t root_length = 0;
    const char* root = lua_tolstring(state, lua_upvalueindex(2), &root_length);
    if (!controlled_file(root, root_length, filename)) {
        lua_pushliteral(state, "\n\tmodule resolved outside the controlled Lua root");
        return 1;
    }
    if (luaL_loadfilex(state, filename, "t") != LUA_OK) {
        return lua_error(state);
    }
    lua_pushstring(state, filename);
    return 2;
}

void copy_global(lua_State* state, const char* name, int environment) {
    lua_getglobal(state, name);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    lua_setfield(state, environment, name);
}

bool field_denied(const char* value, const char* const* denied, size_t denied_count) {
    if (value == nullptr)
        return true;
    for (size_t index = 0; index < denied_count; ++index) {
        if (std::strcmp(value, denied[index]) == 0)
            return true;
    }
    return false;
}

void copy_module(lua_State* state, const char* name, int environment, const char* const* denied,
                 size_t denied_count) {
    lua_getglobal(state, name);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    const int source = lua_gettop(state);
    lua_newtable(state);
    const int target = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, source) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING &&
            !field_denied(lua_tostring(state, -2), denied, denied_count)) {
            lua_pushvalue(state, -2);
            lua_pushvalue(state, -2);
            lua_settable(state, target);
        }
        lua_pop(state, 1);
    }
    lua_setfield(state, environment, name);
    lua_pop(state, 1);
}

void save_package_field(lua_State* state, int package_index, const char* field, int& reference) {
    lua_getfield(state, package_index, field);
    reference = luaL_ref(state, LUA_REGISTRYINDEX);
}

int configure_package_body(lua_State* state) {
    const auto* data = static_cast<const package_path_data*>(lua_touserdata(state, 1));
    if (data->package_ref != LUA_NOREF && data->package_ref != LUA_REFNIL) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, data->package_ref);
    } else {
        lua_getglobal(state, "package");
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    lua_pushlstring(state, data->path, data->length);
    lua_setfield(state, -2, "path");
    lua_pushliteral(state, "");
    lua_setfield(state, -2, "cpath");
    const int package_index = lua_gettop(state);
    lua_newtable(state);
    lua_pushvalue(state, package_index);
    lua_pushlstring(state, data->root, data->root_length);
    lua_pushcclosure(state, controlled_lua_searcher, 2);
    lua_rawseti(state, -2, 1);
    lua_setfield(state, -2, "searchers");
    lua_pop(state, 1);
    return 0;
}

int restore_controlled_package_body(lua_State* state) {
    const auto* data = static_cast<const restore_package_data*>(lua_touserdata(state, 1));
    lua_rawgeti(state, LUA_REGISTRYINDEX, data->package_ref);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return luaL_error(state, "saved package table is unavailable");
    }
    restore_package_ref(state, -1, "path", data->path_ref);
    restore_package_ref(state, -1, "cpath", data->cpath_ref);
    restore_package_ref(state, -1, "searchers", data->searchers_ref);
    lua_pop(state, 1);
    return 0;
}

int arm_body(lua_State* state) {
    auto* values = static_cast<std::pair<sandbox_state*, const lua_sandbox_config*>*>(
        lua_touserdata(state, 1));
    sandbox_state* sandbox = values->first;
    const lua_sandbox_config* config = values->second;

    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    sandbox->original_globals_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    lua_getglobal(state, "package");
    if (lua_istable(state, -1)) {
        const int package_index = lua_gettop(state);
        lua_pushvalue(state, package_index);
        sandbox->original_package_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        save_package_field(state, package_index, "path", sandbox->original_package_path_ref);
        save_package_field(state, package_index, "cpath", sandbox->original_package_cpath_ref);
        save_package_field(state, package_index, "searchers",
                           sandbox->original_package_searchers_ref);
        lua_pushliteral(state, "");
        lua_setfield(state, package_index, "cpath");
        lua_newtable(state);
        lua_pushvalue(state, package_index);
        lua_pushliteral(state, "");
        lua_pushcclosure(state, controlled_lua_searcher, 2);
        lua_rawseti(state, -2, 1);
        lua_setfield(state, package_index, "searchers");
    }
    lua_pop(state, 1);

    lua_newtable(state);
    const int environment = lua_gettop(state);
    static const char* const safe_globals[] = {
        "assert",       "error",        "ipairs",   "next",     "pairs",    "pcall",
        "print",        "rawequal",     "rawget",   "rawlen",   "rawset",   "select",
        "setmetatable", "getmetatable", "tonumber", "tostring", "type",     "unpack",
        "xpcall",       "sao_json",     "sao_log",  "sao_time", "sao_sleep"};
    for (const char* name : safe_globals) {
        copy_global(state, name, environment);
    }
    if (config->allow_coroutine)
        copy_global(state, "coroutine", environment);
    static const char* const denied_string[] = {"dump"};
    if (config->allow_string_lib) {
        copy_module(state, "string", environment, denied_string, 1);
    }
    if (config->allow_math_lib) {
        copy_module(state, "math", environment, nullptr, 0);
    }
    if (config->allow_table_lib) {
        copy_module(state, "table", environment, nullptr, 0);
    }
    static const char* const denied_os[] = {"execute", "exit",      "remove",
                                            "rename",  "setlocale", "tmpname"};
    if (config->allow_process || config->allow_fs) {
        copy_module(state, "os", environment, denied_os, 6);
    }
    static const char* const denied_io[] = {"popen"};
    if (config->allow_fs) {
        copy_module(state, "io", environment, denied_io, 1);
    }
    if (config->allow_require) {
        copy_global(state, "require", environment);
        static const char* const denied_package[] = {"cpath",   "loadlib",   "path",
                                                     "preload", "searchers", "searchpath"};
        copy_module(state, "package", environment, denied_package, 6);
    }
    lua_pushvalue(state, environment);
    lua_setfield(state, environment, "_G");
    if (config->deny_specific_globals != nullptr) {
        for (size_t index = 0; index < config->deny_specific_globals_count; ++index) {
            const char* name = config->deny_specific_globals[index];
            if (name == nullptr)
                continue;
            lua_pushnil(state);
            lua_setfield(state, environment, name);
        }
    }
    lua_pushvalue(state, environment);
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    return 0;
}

void restore_package_ref(lua_State* state, int package_index, const char* field, int reference) {
    if (reference == LUA_NOREF || reference == LUA_REFNIL)
        return;
    package_index = lua_absindex(state, package_index);
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    lua_setfield(state, package_index, field);
}

int restore_body(lua_State* state) {
    auto* sandbox = static_cast<sandbox_state*>(lua_touserdata(state, 1));
    if (sandbox->original_globals_ref != LUA_NOREF && sandbox->original_globals_ref != LUA_REFNIL) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, sandbox->original_globals_ref);
        lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    }
    lua_getglobal(state, "package");
    if (lua_istable(state, -1)) {
        const int package_index = lua_gettop(state);
        restore_package_ref(state, package_index, "path", sandbox->original_package_path_ref);
        restore_package_ref(state, package_index, "cpath", sandbox->original_package_cpath_ref);
        restore_package_ref(state, package_index, "searchers",
                            sandbox->original_package_searchers_ref);
    }
    lua_pop(state, 1);
    return 0;
}

void release_refs(lua_State* state, sandbox_state& sandbox) {
    const int references[] = {sandbox.original_globals_ref, sandbox.original_package_ref,
                              sandbox.original_package_path_ref, sandbox.original_package_cpath_ref,
                              sandbox.original_package_searchers_ref};
    for (const int reference : references) {
        if (reference != LUA_NOREF && reference != LUA_REFNIL) {
            luaL_unref(state, LUA_REGISTRYINDEX, reference);
        }
    }
}

extern "C" void* sandbox_allocator(void* user_data, void* pointer, size_t old_size,
                                   size_t new_size) {
    auto* sandbox = static_cast<sandbox_state*>(user_data);
    if (sandbox == nullptr || sandbox->original_allocator == nullptr) {
        return nullptr;
    }
    if (new_size == 0) {
        if (pointer != nullptr) {
            sandbox->used_bytes =
                sandbox->used_bytes >= old_size ? sandbox->used_bytes - old_size : 0;
        }
        sandbox->original_allocator(sandbox->original_allocator_data, pointer, old_size, 0);
        return nullptr;
    }
    uint64_t next = sandbox->used_bytes;
    if (pointer != nullptr)
        next = next >= old_size ? next - old_size : 0;
    if (new_size > std::numeric_limits<uint64_t>::max() - next) {
        return nullptr;
    }
    next += new_size;
    if (sandbox->max_bytes != 0 && next > sandbox->max_bytes) {
        return nullptr;
    }
    void* result =
        sandbox->original_allocator(sandbox->original_allocator_data, pointer, old_size, new_size);
    if (result != nullptr)
        sandbox->used_bytes = next;
    return result;
}

extern "C" void sandbox_count_hook(lua_State* state, lua_Debug*) {
    luaL_error(state, "sao_lua_sandbox: instruction limit reached");
}

} // namespace

namespace detail {

bool sandbox_is_armed_locked(lua_State* state) noexcept {
    try {
        std::lock_guard map_lock(g_sandboxes_mutex);
        return g_sandboxes.contains(state);
    } catch (...) {
        return false;
    }
}

bool sandbox_stdlib_allowed_locked(lua_State* state) noexcept {
    return !sandbox_is_armed_locked(state);
}

int32_t sandbox_add_controlled_package_path_locked(lua_State* state, const char* path,
                                                   size_t path_length, const char* root,
                                                   size_t root_length,
                                                   package_path_snapshot& snapshot) noexcept {
    if (state == nullptr || path == nullptr || root == nullptr || snapshot.layer_id != 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    state = main_thread(state);
    try {
        package_path_layers candidate;
        bool first_layer = false;
        {
            std::lock_guard layers_lock(g_package_layers_mutex);
            first_layer = !g_package_layers.contains(state);
        }

        if (first_layer) {
            {
                std::lock_guard map_lock(g_sandboxes_mutex);
                const auto found = g_sandboxes.find(state);
                if (found != g_sandboxes.end() &&
                    found->second->original_package_ref != LUA_NOREF &&
                    found->second->original_package_ref != LUA_REFNIL) {
                    lua_rawgeti(state, LUA_REGISTRYINDEX, found->second->original_package_ref);
                } else {
                    lua_getglobal(state, "package");
                }
            }
            if (!lua_istable(state, -1)) {
                lua_pop(state, 1);
                return SAO_OK;
            }
            const int package_index = lua_gettop(state);
            lua_pushvalue(state, package_index);
            candidate.package_ref = luaL_ref(state, LUA_REGISTRYINDEX);
            save_package_field(state, package_index, "path", candidate.path_ref);
            save_package_field(state, package_index, "cpath", candidate.cpath_ref);
            save_package_field(state, package_index, "searchers", candidate.searchers_ref);
            lua_pop(state, 1);
        }

        package_path_data data{path, path_length, LUA_NOREF, root, root_length};
        if (first_layer) {
            data.package_ref = candidate.package_ref;
        } else {
            std::lock_guard layers_lock(g_package_layers_mutex);
            const auto found = g_package_layers.find(state);
            if (found == g_package_layers.end())
                return SAO_ERR_HANDLE_INVALID;
            data.package_ref = found->second.package_ref;
        }
        if (protected_trampoline(state, configure_package_body, &data, 0) != LUA_OK) {
            capture_state_error_locked(state, -1);
            lua_pop(state, 1);
            for (const int reference : {candidate.package_ref, candidate.path_ref,
                                        candidate.cpath_ref, candidate.searchers_ref}) {
                if (reference != LUA_NOREF && reference != LUA_REFNIL)
                    luaL_unref(state, LUA_REGISTRYINDEX, reference);
            }
            return SAO_ERR_OS_CALL_FAILED;
        }

        package_path_layer layer;
        layer.path.assign(path, path_length);
        layer.root.assign(root, root_length);
        {
            std::lock_guard layers_lock(g_package_layers_mutex);
            if (first_layer) {
                auto [entry, inserted] = g_package_layers.emplace(state, std::move(candidate));
                if (!inserted)
                    return SAO_ERR_OS_CALL_FAILED;
            }
            auto& layers = g_package_layers.at(state);
            do {
                layer.id = g_next_package_layer_id++;
            } while (layer.id == 0);
            layers.layers.push_back(std::move(layer));
            snapshot.layer_id = layers.layers.back().id;
        }
        return SAO_OK;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t sandbox_restore_package_path_locked(lua_State* state,
                                            package_path_snapshot& snapshot) noexcept {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
    if (snapshot.layer_id == 0) {
        return SAO_OK;
    }
    state = main_thread(state);
    try {
        std::lock_guard layers_lock(g_package_layers_mutex);
        const auto found = g_package_layers.find(state);
        if (found == g_package_layers.end())
            return SAO_ERR_HANDLE_INVALID;
        auto& owned = found->second;
        const auto layer =
            std::find_if(owned.layers.begin(), owned.layers.end(),
                         [&snapshot](const auto& item) { return item.id == snapshot.layer_id; });
        if (layer == owned.layers.end())
            return SAO_ERR_HANDLE_INVALID;

        const auto next = layer == std::prev(owned.layers.end()) && owned.layers.size() > 1
                              ? std::prev(layer)
                              : owned.layers.end();
        int32_t status = SAO_OK;
        if (owned.layers.size() == 1) {
            restore_package_data data{owned.package_ref, owned.path_ref, owned.cpath_ref,
                                      owned.searchers_ref};
            if (protected_trampoline(state, restore_controlled_package_body, &data, 0) != LUA_OK) {
                capture_state_error_locked(state, -1);
                lua_pop(state, 1);
                return SAO_ERR_OS_CALL_FAILED;
            }
        } else if (layer == std::prev(owned.layers.end())) {
            package_path_data data{next->path.data(), next->path.size(), owned.package_ref,
                                   next->root.data(), next->root.size()};
            if (protected_trampoline(state, configure_package_body, &data, 0) != LUA_OK) {
                capture_state_error_locked(state, -1);
                lua_pop(state, 1);
                return SAO_ERR_OS_CALL_FAILED;
            }
        }

        owned.layers.erase(layer);
        snapshot = {};
        if (!owned.layers.empty())
            return status;
        for (const int reference :
             {owned.package_ref, owned.path_ref, owned.cpath_ref, owned.searchers_ref}) {
            if (reference != LUA_NOREF && reference != LUA_REFNIL)
                luaL_unref(state, LUA_REGISTRYINDEX, reference);
        }
        g_package_layers.erase(found);
        return status;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

int32_t sandbox_disarm_locked(lua_State* state) noexcept {
    try {
        std::unique_ptr<sandbox_state> sandbox;
        {
            std::lock_guard map_lock(g_sandboxes_mutex);
            const auto found = g_sandboxes.find(state);
            if (found == g_sandboxes.end())
                return SAO_ERR_INVALID_ARGUMENT;
            sandbox = std::move(found->second);
            g_sandboxes.erase(found);
        }
        lua_sethook(state, sandbox->original_hook, sandbox->original_hook_mask,
                    sandbox->original_hook_count);
        if (sandbox->max_bytes != 0) {
            lua_setallocf(state, sandbox->original_allocator, sandbox->original_allocator_data);
        }
        const int restore_status = protected_trampoline(state, restore_body, sandbox.get(), 0);
        if (restore_status != LUA_OK) {
            capture_state_error_locked(state, -1);
            lua_pop(state, 1);
        }
        release_refs(state, *sandbox);
        return restore_status == LUA_OK ? SAO_OK : SAO_ERR_OS_CALL_FAILED;
    } catch (...) {
        return SAO_ERR_OS_CALL_FAILED;
    }
}

} // namespace detail

#endif

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_arm(lua_State* state, const lua_sandbox_config* config) {
    if (state == nullptr || config == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    detail::state_operation operation;
    std::unique_ptr<sandbox_state> sandbox;
    int base = 0;
    try {
        int32_t status = detail::acquire_state_operation(state, operation);
        if (status != SAO_OK)
            return status;
        state = operation.state();
        base = lua_gettop(state);
        if (detail::sandbox_is_armed_locked(state)) {
            return SAO_ERR_INVALID_ARGUMENT;
        }
        sandbox = std::make_unique<sandbox_state>();
        sandbox->original_allocator = lua_getallocf(state, &sandbox->original_allocator_data);
        sandbox->original_hook = lua_gethook(state);
        sandbox->original_hook_mask = lua_gethookmask(state);
        sandbox->original_hook_count = lua_gethookcount(state);
        sandbox->max_bytes = config->max_memory_bytes;
        std::pair<sandbox_state*, const lua_sandbox_config*> values{sandbox.get(), config};
        if (detail::protected_trampoline(state, arm_body, &values, 0) != LUA_OK) {
            detail::capture_state_error_locked(state, -1);
            lua_settop(state, base);
            (void)detail::protected_trampoline(state, restore_body, sandbox.get(), 0);
            lua_settop(state, base);
            release_refs(state, *sandbox);
            return SAO_ERR_OS_CALL_FAILED;
        }
        if (sandbox->max_bytes != 0) {
            const uint64_t kilobytes =
                static_cast<uint64_t>(std::max(lua_gc(state, LUA_GCCOUNT), 0));
            const uint64_t remainder =
                static_cast<uint64_t>(std::max(lua_gc(state, LUA_GCCOUNTB), 0));
            sandbox->used_bytes = kilobytes * 1024 + remainder;
        }
        sandbox_state* installed = sandbox.get();
        {
            std::lock_guard map_lock(g_sandboxes_mutex);
            if (g_sandboxes.contains(state)) {
                (void)detail::protected_trampoline(state, restore_body, installed, 0);
                lua_settop(state, base);
                release_refs(state, *installed);
                return SAO_ERR_INVALID_ARGUMENT;
            }
            g_sandboxes.reserve(g_sandboxes.size() + 1);
            auto [entry, inserted] = g_sandboxes.emplace(state, nullptr);
            if (!inserted)
                return SAO_ERR_INVALID_ARGUMENT;
            entry->second = std::move(sandbox);
        }
        if (config->max_memory_bytes != 0) {
            lua_setallocf(state, sandbox_allocator, installed);
        }
        if (config->max_instructions_per_run != 0) {
            const uint32_t maximum = static_cast<uint32_t>(std::numeric_limits<int>::max());
            const int count = static_cast<int>(std::min(config->max_instructions_per_run, maximum));
            lua_sethook(state, sandbox_count_hook, LUA_MASKCOUNT, count);
        }
        return SAO_OK;
    } catch (...) {
        if (sandbox != nullptr && operation) {
            (void)detail::protected_trampoline(state, restore_body, sandbox.get(), 0);
            lua_settop(state, base);
            release_refs(state, *sandbox);
        }
        return SAO_ERR_OS_CALL_FAILED;
    }
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_disarm(lua_State* state) {
    if (state == nullptr)
        return SAO_ERR_INVALID_ARGUMENT;
#if defined(SAO_HAS_LUA)
    detail::state_operation operation;
    const int32_t status = detail::acquire_state_operation(state, operation);
    return status == SAO_OK ? detail::sandbox_disarm_locked(operation.state()) : status;
#else
    return SAO_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" SAO_PLUGINS_API uint64_t SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_bytes_used(lua_State* state) {
    if (state == nullptr)
        return 0;
#if defined(SAO_HAS_LUA)
    try {
        detail::state_operation operation;
        if (detail::acquire_state_operation(state, operation) != SAO_OK) {
            return 0;
        }
        state = operation.state();
        std::lock_guard map_lock(g_sandboxes_mutex);
        const auto found = g_sandboxes.find(state);
        return found == g_sandboxes.end() ? 0 : found->second->used_bytes;
    } catch (...) {
        return 0;
    }
#else
    return 0;
#endif
}

extern "C" SAO_PLUGINS_API bool SAO_PLUGINS_CALL
sao_plugins_luahost_sandbox_is_armed(lua_State* state) {
    if (state == nullptr)
        return false;
#if defined(SAO_HAS_LUA)
    return detail::sandbox_is_armed_locked(state);
#else
    return false;
#endif
}

} // namespace sao::plugins::lua_host
